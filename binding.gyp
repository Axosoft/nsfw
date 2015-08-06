{
  "targets": [
    {
      "target_name": "FileWatcher",
      "sources": [
        "src/FileWatcher.cpp",
        "src/NodeSentinelFileWatcher.cpp",
        "src/FileWatcher32.cpp",
        "includes/FileWatcher.h",
        "includes/NodeSentinelFileWatcher.h",
        "includes/FileWatcherInterface.h",
        "includes/FileWatcher32.h"
      ],
      "include_dirs": [
        "<!(node -e \"require('nan')\")",
        "includes"
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
    }
  ]
}
