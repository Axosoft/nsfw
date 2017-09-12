{
    "targets": [{
        "target_name": "nsfw",

        "sources": [
            "src/NSFW.cpp",
            "src/Queue.cpp",
            "src/NativeInterface.cpp",
            "includes/NSFW.h",
            "includes/Queue.h",
            "includes/NativeInterface.h"
        ],
        "win_delay_load_hook": "false",
        "include_dirs": [
            "<!(node -e \"require('nan')\")",
            "includes"
        ],
        "conditions": [
            ["OS=='win'", {
                "sources": [
                    "src/win32/ReadLoop.cpp",
                    "src/win32/ReadLoopRunner.cpp",
                    "includes/win32/ReadLoop.h",
                    "includes/win32/ReadLoopRunner.h",
                ],
                "msvs_settings": {
                    "VCCLCompilerTool": {
                        "DisableSpecificWarnings": [ "4506", "4538", "4793" ]
                    },
                    "VCLinkerTool": {
                        "AdditionalOptions": [ "/ignore:4248" ]
                    }
                }
            }],
            ["OS=='mac'", {
                "sources": [
                    "src/Lock.cpp",
                    "src/osx/RunLoop.cpp",
                    "src/osx/FSEventsService.cpp",
                    "includes/Lock.h",
                    "includes/osx/RunLoop.h",
                    "includes/osx/FSEventsService.h"
                ],
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
            ["OS=='linux'", {
                "sources": [
                    "src/Lock.cpp",
                    "src/linux/InotifyEventLoop.cpp",
                    "src/linux/InotifyTree.cpp",
                    "src/linux/InotifyService.cpp",
                    "includes/Lock.h",
                    "includes/linux/InotifyEventLoop.h",
                    "includes/linux/InotifyTree.h",
                    "includes/linux/InotifyService.h"
                ],
                "cflags": [
                    "-Wno-unknown-pragmas",
                    "-std=c++0x"
                ]
            }],
            ["OS=='win'", {
                "conditions": [
                    ["target_arch=='x64'", {
                        "VCLibrarianTool": {
                          "AdditionalOptions": [
                            "/MACHINE:X64",
                          ],
                        },
                    }, {
                        "VCLibrarianTool": {
                          "AdditionalOptions": [
                            "/MACHINE:x86",
                          ],
                        },
                    }],
                ]
            }],
            ["OS=='mac' or OS=='linux'", {
                "defines": [
                    "HAVE_STDDEF_H=1",
                    "HAVE_STDLIB_H=1",
                    "HAVE_UNISTD_H=1"
                ]
            }],
        ],
    }]
}
