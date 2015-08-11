{
  "targets": [
    {
      "target_name": "FileWatcher",
      "sources": [
        "src/FileWatcher.cpp",
        "src/NodeSentinelFileWatcher.cpp",
        "includes/FileWatcher.h",
        "includes/NodeSentinelFileWatcher.h",
        "includes/FileWatcherInterface.h"
      ],
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
                    ]
                }
            }
        }],
        ["OS=='linux'", {
            "sources": [
            "src/FileWatcherLinux.cpp",
            "includes/FileWatcherLinux.h"
            ]
        }]
      ]
    }
  ]
}
