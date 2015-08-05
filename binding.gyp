{
  "targets": [
    {
      "target_name": "FileWatcher",
      "sources": [
        "src/FileWatcher.cpp",
        "src/NodeSentinelFileWatcher.cpp",
        "includes/FileWatcher.h",
        "includes/NodeSentinelFileWatcher.h"
      ],
      "include_dirs": [
        "<!(node -e \"require('nan')\")",
        "includes"
      ]
    }
  ]
}
