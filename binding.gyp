{
    "targets": [{
        "target_name": "nsfw",
        "defines": [
            "NODE_API_SWALLOW_UNTHROWABLE_EXCEPTIONS"
        ],
        "sources": [
            "src/NSFW.cpp",
            "src/Queue.cpp",
            "src/NativeInterface.cpp"
        ],
        "include_dirs": [
            "includes",
            "<!(node -p \"require('node-addon-api').include_dir\")"
        ],
        "cflags!": ["-fno-exceptions"],
        "cflags_cc!": ["-fno-exceptions"],
        "xcode_settings": {
            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
            "CLANG_CXX_LIBRARY": "libc++",
            "MACOSX_DEPLOYMENT_TARGET": "10.7",
        },
        "msvs_settings": {
            "VCCLCompilerTool": { "ExceptionHandling": 1 },
        },
        "conditions": [
            ["OS=='win'", {
                "sources": [
                    "src/win32/Controller.cpp",
                    "src/win32/Watcher.cpp"
                ],
                "msvs_settings": {
                    "VCCLCompilerTool": {
                        "DisableSpecificWarnings": [ ]
                    },
                    "VCLinkerTool": {
                        "AdditionalOptions": [ ]
                    }
                }
            }],
            ["OS=='mac'", {
                "cflags+": ["-fvisibility-hidden"],
                "sources": [
                    "src/osx/RunLoop.cpp",
                    "src/osx/FSEventsService.cpp"
                ],
                "xcode_settings": {
                    "GCC_SYMBOLS_PRIVATE_EXTERN": "YES", # -fvisibility=hidden
                    "MACOSX_DEPLOYMENT_TARGET": "10.7",
                    # Pinned because common.gypi only sets it when clang==1, and with --nodedir or --dist-url node-gyp
                    # reads clang from the headers' config.gypi (0 for Node 22 and 24), so the same Node built as
                    # gnu++20 or as Apple clang's C++14 default depending on the install route. napi.h needs C++17.
                    "CLANG_CXX_LANGUAGE_STANDARD": "gnu++17", # -std=gnu++17
                },
                "link_settings": {
                    "xcode_settings": {
                        "OTHER_LDFLAGS": [
                            "-framework CoreServices"
                        ],
                        "OTHER_CFLAGS": [
                            "-Wno-unknown-pragmas"
                        ]
                    }
                }
            }],
            ["OS=='linux' or OS=='freebsd'", {
                "defines": [
                    "NSFW_TEST_SLOW_<!(node -p process.env.NSFW_TEST_SLOW)"
                ],
                "sources": [
                    "src/linux/InotifyEventLoop.cpp",
                    "src/linux/InotifyTree.cpp",
                    "src/linux/InotifyService.cpp"
                ],
                "cflags": [
                    "-Wno-unknown-pragmas"
                ]
            }],
            ["OS=='win'", {
                "conditions": [
                    ["target_arch=='x64'", {
                        "VCLibrarianTool": {
                          "AdditionalOptions": [
                            "/MACHINE:X64"
                          ]
                        }
                    }, {
                        "VCLibrarianTool": {
                          "AdditionalOptions": [
                            "/MACHINE:x86"
                          ]
                        }
                    }]
                ]
            }],
            ["OS=='mac' or OS=='linux' or OS=='freebsd'", {
                "defines": [
                    "HAVE_STDDEF_H=1",
                    "HAVE_STDLIB_H=1",
                    "HAVE_UNISTD_H=1"
                ]
            }],
            ["OS=='freebsd'", {
                "include_dirs": [
                    "/usr/local/include"
                ],
                "libraries": [
                    "-L/usr/local/lib",
                    "-linotify"
                ]
            }],
        ]
    }]
}
