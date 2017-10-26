#ifndef INOTIFY_TREE_H
#define INOTIFY_TREE_H
#include <sys/inotify.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <sstream>
#include <vector>
#include <map>

class InotifyTree {
public:
  InotifyTree(int inotifyInstance, std::string path);

  void addDirectory(int wd, std::string name);
  std::string getError();
  bool getPath(std::string &out, int wd);
  bool hasErrored();
  bool isRootAlive();
  bool nodeExists(int wd);
  void removeDirectory(int wd);
  void renameDirectory(int wd, std::string oldName, std::string newName);

  ~InotifyTree();
private:
  class InotifyNode {
  public:
    InotifyNode(
      InotifyTree *tree,
      int inotifyInstance,
      InotifyNode *parent,
      std::string directory,
      std::string name
    );

    void addChild(std::string name);
    void fixPaths();
    std::string getFullPath();
    std::string getName();
    InotifyNode *getParent();
    bool inotifyInit();
    bool isAlive();
    void removeChild(std::string name);
    void renameChild(std::string oldName, std::string newName);
    void setName(std::string name);

    ~InotifyNode();
  private:
    static std::string createFullPath(std::string parentPath, std::string name);
    static const int ATTRIBUTES = IN_ATTRIB
                                | IN_CREATE
                                | IN_DELETE
                                | IN_MODIFY
                                | IN_MOVED_FROM
                                | IN_MOVED_TO
                                | IN_DELETE_SELF;

    bool mAlive;
    std::map<std::string, InotifyNode *> *mChildren;
    std::string mDirectory;
    std::string mFullPath;
    const int mInotifyInstance;
    std::string mName;
    InotifyNode *mParent;
    InotifyTree *mTree;
    int mWatchDescriptor;
    bool mWatchDescriptorInitialized;
  };

  void setError(std::string error);
  void addNodeReferenceByWD(int watchDescriptor, InotifyNode *node);
  void removeNodeReferenceByWD(int watchDescriptor);

  std::string mError;
  const int mInotifyInstance;
  std::map<int, InotifyNode *> *mInotifyNodeByWatchDescriptor;
  InotifyNode *mRoot;

  friend class InotifyNode;
};

#endif
