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
        "include_dirs": [
            "<!(node -e \"require('nan')\")",
            "includes"
        ],
        "conditions": [
            ["OS=='win'", {
                "sources": [
                    "src/win32/Controller.cpp",
                    "src/win32/Watcher.cpp",
                    "includes/win32/Controller.h",
                    "includes/win32/Watcher.h"
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
                    "src/osx/RunLoop.cpp",
                    "src/osx/FSEventsService.cpp",
                    "includes/osx/RunLoop.h",
                    "includes/osx/FSEventsService.h"
                ],
                "xcode_settings": {
                    'MACOSX_DEPLOYMENT_TARGET': '10.7',
                    "OTHER_CFLAGS": [
                        "-std=c++11",
                        "-stdlib=libc++"
                    ],
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
                "sources": [
                    "src/linux/InotifyEventLoop.cpp",
                    "src/linux/InotifyTree.cpp",
                    "src/linux/InotifyService.cpp",
                    "includes/linux/InotifyEventLoop.h",
                    "includes/linux/InotifyTree.h",
                    "includes/linux/InotifyService.h"
                ],
                "cflags": [
                    "-Wno-unknown-pragmas",
                    "-std=c++11"
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
                ]
            }],
        ]
    }]
}
