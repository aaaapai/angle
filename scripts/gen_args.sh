#!/usr/bin/env bash
#
# gen_args.sh <platform> <arch> <mode> <out-dir>
#
#   platform : android | ios | linux
#   arch     : arm64 | arm32 | x86 | x86_64
#   mode     : release | debug | fastdebug
#   out-dir  : GN 输出目录名，例如 Android-arm64-fastdebug
#
# 流程：
#   1. 从 out/args.gn.in 按 section 优先级合并 -> 扁平 key=value
#        优先级（高→低）：
#          platform-<X>-<ARCH>  >  platform-<X>-<MODE>  >  platform-<X>
#          >  mode-<X>  >  arch-<X>  >  base
#        即同名 key 出现在多个 section 时，优先级最高的生效
#   2. release 且非 iOS 时，把 #@POLLY@ 占位符替换为 polly 插件参数
#      其余情况删除该占位行
#   3. 写出 out/<out-dir>/args.gn（纯 key=value，GN 可直接解析）
#
# 设计约束：
#   - mode 与平台/架构解耦：release / debug / fastdebug 三种模式在全平台
#     全架构通用。某平台/架构是否需要屏蔽某个 mode 特有的编译开关，
#     由 args.gn.in 的 platform-* / arch-* section 覆盖来完成，脚本不再
#     硬编码平台白名单。这样后续新增平台/架构或 mode 都不会牵连本脚本。
#
set -euo pipefail

PLATFORM="${1:?usage: gen_args.sh <platform> <arch> <mode> <out-dir>}"
ARCH="${2:?}"
MODE="${3:?}"
OUT_DIR="${4:?}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TEMPLATE="$REPO_ROOT/out/args.gn.in"
OUT_FILE="$REPO_ROOT/out/$OUT_DIR/args.gn"

if [ ! -f "$TEMPLATE" ]; then
  echo "[gen_args] 找不到模板: $TEMPLATE" >&2
  exit 1
fi

# ---------- 参数校验 ----------
case "$MODE" in
  release|debug|fastdebug) ;;
  *) echo "[gen_args] 未知模式: $MODE" >&2; exit 1 ;;
esac

case "$PLATFORM" in
  android|ios|linux) ;;
  *) echo "[gen_args] 未知平台: $PLATFORM" >&2; exit 1 ;;
esac

case "$ARCH" in
  x86_64|x64) ARCH_KEY=x64 ;;
  arm64)      ARCH_KEY=arm64 ;;
  arm32)      ARCH_KEY=arm32 ;;
  x86)        ARCH_KEY=x86 ;;
  *) echo "[gen_args] 未知架构: $ARCH" >&2; exit 1 ;;
esac

mkdir -p "$(dirname "$OUT_FILE")"

# ============================================================
#  步骤 1：按 section 优先级合并 -> 扁平 key=value
#
#  priority 列表（逗号分隔，越靠前优先级越高）：
#    platform-<X>-<ARCH>, platform-<X>-<MODE>, platform-<X>, mode-<X>, arch-<X>, base
#
#  platform-<X>-<ARCH> 用于 platform 内部按架构细分的差异
#  （典型例子：iOS 真机 vs iOS 模拟器的 target_environment）。
#  platform-<X>-<MODE> 用于 platform 在某个构建模式下需要偏离通用 mode-<X>
#  的差异（典型例子：iOS fastdebug 打开 Vulkan 验证层，而 platform-ios 默认关闭）。
#  这两个 section 都可以不存在，匹配不到只是不生效，不影响其它 section。
# ============================================================
PRIORITY="platform-$PLATFORM-$ARCH_KEY,platform-$PLATFORM-$MODE,platform-$PLATFORM,mode-$MODE,arch-$ARCH_KEY,base"

awk -v priority="$PRIORITY" '
BEGIN {
  n = split(priority, arr, ",")
  for (i = 1; i <= n; i++) want[arr[i]] = i   # priority 值：数字小=优先级高
  cur_pri = 0
  cur_key = ""
  cur_val = ""
  depth = 0
  count = 0
}

function count_depth(s,   tmp, o, c, o2, c2) {
  tmp = s; o = gsub(/\[/, "", tmp)
  tmp = s; c = gsub(/\]/, "", tmp)
  tmp = s; o2 = gsub(/\{/, "", tmp)
  tmp = s; c2 = gsub(/\}/, "", tmp)
  return (o - c) + (o2 - c2)
}

function emit(k, v, p) {
  if (!(k in key_pri)) {
    order[++count] = k
    key_val[k] = v
    key_pri[k] = p
  } else if (p < key_pri[k]) {
    key_val[k] = v
    key_pri[k] = p
  }
}

function flush_pending() {
  if (cur_key != "") {
    emit(cur_key, cur_val, cur_pri)
    cur_key = ""; cur_val = ""; depth = 0
  }
}

# section 头
/^# ==SECTION:/ {
  flush_pending()
  s = $0
  sub(/^# ==SECTION:/, "", s)
  sub(/==$/, "", s)
  gsub(/^[ \t]+|[ \t]+$/, "", s)
  cur_pri = (s in want) ? want[s] : 0
  next
}

# 不在目标 section 内，跳过
{
  if (cur_pri == 0) next
}

# 累积 key / value（支持数组跨行）
{
  if (cur_key == "") {
    if (match($0, /^[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*=/)) {
      eq = index($0, "=")
      key = substr($0, 1, eq - 1)
      gsub(/[ \t]/, "", key)
      cur_key = key
      cur_val = $0
      depth = count_depth($0)
      if (depth <= 0) {
        emit(cur_key, cur_val, cur_pri)
        cur_key = ""; cur_val = ""; depth = 0
      }
    }
  } else {
    cur_val = cur_val "\n" $0
    depth += count_depth($0)
    if (depth <= 0) {
      emit(cur_key, cur_val, cur_pri)
      cur_key = ""; cur_val = ""; depth = 0
    }
  }
}

END {
  flush_pending()
  print "# 由 scripts/gen_args.sh 自动生成"
  print "# platform=" priority
  print ""
  for (i = 1; i <= count; i++) {
    print key_val[order[i]]
    print ""
  }
}
' "$TEMPLATE" > "$OUT_FILE.tmp"

# ============================================================
#  步骤 2：polly 占位符替换
#   条件：mode == release 且 platform != ios
#   插件路径优先级：
#     1) 环境变量 POLLY_PLUGIN_PATH
#     2) chromium llvm-build 内置 LLVMPolly.so / libLLVMPolly.so
#   找不到则视为无 polly（占位行删除）
# ============================================================
POLLY_LINES=""
if [ "$MODE" = "release" ] && [ "$PLATFORM" != "ios" ]; then
  POLLY_PLUGIN="${POLLY_PLUGIN_PATH:-}"
  if [ -z "$POLLY_PLUGIN" ] || [ ! -f "$POLLY_PLUGIN" ]; then
    for f in \
      "$REPO_ROOT/third_party/llvm-build/Release+Asserts/lib/LLVMPolly.so" \
      "$REPO_ROOT/third_party/llvm-build/Release+Asserts/lib/libLLVMPolly.so"; do
      if [ -f "$f" ]; then POLLY_PLUGIN="$f"; break; fi
    done
  fi

  if [ -n "$POLLY_PLUGIN" ] && [ -f "$POLLY_PLUGIN" ]; then
    echo "[gen_args] polly: 使用插件 $POLLY_PLUGIN"
    # 注意：awk 替换时保留每行缩进
    POLLY_LINES=$(cat <<EOF
  "-fpass-plugin=$POLLY_PLUGIN",
  "-mllvm",
  "-polly",
  "-mllvm",
  "-polly-vectorizer=stripmine",
EOF
)
  else
    echo "[gen_args] polly: 未找到 LLVMPolly.so，跳过（可通过 POLLY_PLUGIN_PATH 指定）"
  fi
fi

awk -v repl="$POLLY_LINES" '
  /^[ \t]*#@POLLY@[ \t]*$/ {
    if (repl != "") print repl
    next
  }
  { print }
' "$OUT_FILE.tmp" > "$OUT_FILE"

rm -f "$OUT_FILE.tmp"

echo "[gen_args] 已生成: $OUT_FILE (platform=$PLATFORM arch=$ARCH mode=$MODE)"
