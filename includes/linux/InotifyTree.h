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
  InotifyTree(int inotifyInstance, int attributes, std::string path);

  void addDirectory(int wd, std::string name);
  bool getPath(std::string &out, int wd);
  bool isRootAlive();
  void removeDirectory(int wd);
  void renameDirectory(int wd, std::string oldName, std::string newName);

  ~InotifyTree();
private:
  class InotifyNode {
  public:
    InotifyNode(
      int attributes,
      std::map<int, InotifyNode *> *inotifyNodeByWatchDescriptor,
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

    bool mAlive;
    const int mAttributes;
    std::map<std::string, InotifyNode *> *mChildren;
    std::string mDirectory;
    std::string mFullPath;
    const int mInotifyInstance;
    std::map<int, InotifyNode *> *mInotifyNodeByWatchDescriptor;
    std::string mName;
    InotifyNode *mParent;
    int mWatchDescriptor;
    bool mWatchDescriptorInitialized;
  };

  const int mAttributes;
  const int mInotifyInstance;
  std::map<int, InotifyNode *> *mInotifyNodeByWatchDescriptor;
  InotifyNode *mRoot;
};

#endif
