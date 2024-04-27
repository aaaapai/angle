# This file exists purely to prevent Android.mk files in subdirectories from being automatically used
# See http://b/188491905 for more details
LOCAL_CFLAGS += -DANDROID -pipe -integrated-as -fno-plt -Ofast -flto -mllvm -polly -mllvm -polly-vectorizer=stripmine -mllvm -polly-invariant-load-hoisting -mllvm -polly-run-inliner -mllvm -polly-run-dce
