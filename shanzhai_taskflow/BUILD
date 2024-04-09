cc_library(
  name = "shanzhai_taskflow",
  srcs = glob([
    "taskflow/core/*.hpp",
  ]),
  includes = [
    '.',
  ],
  copts = [
    "-Wall",
    "-Werror",
    "-std=c++20",
  ],
  visibility = ["//visibility:public"],
)

cc_binary(
  name = 'notifier',
  srcs = [
    'examples/notifier.cpp',
  ],
  deps = [
    ":shanzhai_taskflow",
  ],
  copts = [
   '-Wall',
   '-Werror',
   '-std=c++17',
  ],
  linkopts = [
    "-lpthread",
  ],
)
