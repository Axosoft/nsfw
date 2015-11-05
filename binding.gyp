{
    "targets": [{
        "target_name": "FileWatcher",

        "dependencies": [
            "openpa/openpa.gyp:openpa"
        ],

        "sources": [
            "src/FileWatcher.cpp",
            "src/NodeSentinelFileWatcher.cpp",
            "src/EventQueue.cpp",
            "includes/FileWatcher.h",
            "includes/NodeSentinelFileWatcher.h",
            "includes/EventQueue.h"
        ],
        "win_delay_load_hook": "false",
        "include_dirs": [
            "<!(node -e \"require('nan')\")",
            "includes"
        ],
        "conditions": [
            ["OS=='win'", {
                "sources": [
                    "src/FileWatcher32.cpp",
                    "includes/FileWatcher32.h"
                ],
                "msvs_settings": {
                    "VCCLCompilerTool": {
                        "AdditionalOptions": [ "/clr" ],
                        "DisableSpecificWarnings": [ "4506" ]
                    },
                    "VCLinkerTool": {
                        "AdditionalOptions": [ "/ignore:4248" ]
                    }
                },
                "configurations" : {
                    "Release": {
                        "msvs_settings": {
                            "VCCLCompilerTool": {
                                "RuntimeLibrary": 2,
                                "RuntimeTypeInfo": "true"
                            },
                            "VCLinkerTool": {
                                "LinkTimeCodeGeneration": 0
                            }
                        }
                    },
                    "Debug": {
                        "msvs_settings": {
                            "VCCLCompilerTool": {
                                "RuntimeLibrary": 3,
                                "BasicRuntimeChecks": 0,
                                "ExceptionHandling": 0
                            }
                        }
                    }
                }
            }],
            ["OS=='mac'", {
                "sources": [
                    "src/FileWatcherOSX.cpp",
                    "includes/FileWatcherOSX.h"
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
                    "src/FileWatcherLinux.cpp",
                    "includes/FileWatcherLinux.h"
                ],
                "cflags": [
                    "-Wno-unknown-pragmas"
                ]
            }],
            ["OS=='win'", {
                "defines": [
                    "OPA_HAVE_NT_INTRINSICS=1",
                    "_opa_inline=__inline"
                ],
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
                    "OPA_HAVE_GCC_INTRINSIC_ATOMICS",
                    "OPA_HAVE_STDDEF_H"
                ]
            }],
            ["target_arch=='x64' or target_arch=='arm64'", {
                "defines": [
                    "OPA_SIZEOF_VOID_P=8"
                ]
            }],
            ["target_arch=='ia32' or target_arch=='armv7'", {
                "defines": [
                    "OPA_SIZEOF_VOID_P=4"
                ]
            }]
        ],
    }]
}
