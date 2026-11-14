# Metal ES 3.1 translator corpus (L1)

Run the standalone translator over these shaders once `angle_shader_translator` is built:

```
angle_shader_translator -s=e31 -b=m positive/compute_empty.comp -o /tmp/compute_empty.metal
```

Positive cases must succeed and emit MSL.  Negative cases must fail with a diagnostic that
contains the GLSL line and the `MTL31-MSLLIMIT` ID.

The corpus is intentionally not wired into `BUILD.gn` yet: the CI runner for L1/L3 still needs
the `msl-compile` job described in `06-验证与测试.md`.
