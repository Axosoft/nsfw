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
#include <unordered_set>
#include <functional>
#include <chrono>
#include <thread>

/**
 * void EmitCreatedEvent(std::string directory, std::string file);
 */
using EmitCreatedEvent = std::function<void(std::string, std::string)>;

class InotifyTree {
public:
  InotifyTree(int inotifyInstance, std::string path, const std::vector<std::string> &excludedPaths);

  void addDirectory(int wd, std::string name, EmitCreatedEvent emitCreatedEvent = nullptr);
  std::string getError();
  bool getPath(std::string &out, int wd);
  bool hasErrored();
  bool isRootAlive();
  bool nodeExists(int wd);
  void removeDirectory(int wd);
  void renameDirectory(int fromWd, std::string fromName, int toWd, std::string toName);
  const std::vector<std::string>& getExcludedPaths() const;
  void updateExcludedPaths(const std::vector<std::string> &excludedPaths);

  ~InotifyTree();
private:
  class InotifyNode {
  public:
    InotifyNode(
      InotifyTree *tree,
      int inotifyInstance,
      InotifyNode *parent,
      const std::string &directory,
      std::string name,
      ino_t inodeNumber,
      EmitCreatedEvent emitCreatedEvent = nullptr
    );

    void addChild(std::string name, EmitCreatedEvent emitCreatedEvent = nullptr);
    void fixPaths();
    std::string getFullPath();
    std::string getName();
    InotifyNode *getParent();
    bool inotifyInit();
    bool isAlive();
    InotifyNode *pullChild(std::string name);
    void removeChild(std::string name);
    void renameChild(std::string oldName, std::string newName);
    void setName(std::string name);
    void setParent(InotifyNode *newParent);
    void takeChildAsName(InotifyNode *child, std::string name);

    ~InotifyNode();
  private:
    static std::string createFullPath(std::string parentPath, std::string name);
    static const int ATTRIBUTES = IN_ATTRIB
                                | IN_CREATE
                                | IN_DELETE
                                | IN_MODIFY
                                | IN_MOVED_FROM
                                | IN_MOVED_TO
                                | IN_DELETE_SELF
                                | IN_ONLYDIR;

    bool mAlive;
    std::map<std::string, InotifyNode *> *mChildren;
    const std::string &mDirectory;
    std::string mFullPath;
    ino_t mInodeNumber;
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
  bool addInode(ino_t inodeNumber, InotifyNode *node);
  void removeInode(ino_t inodeNumber);
  InotifyNode * findNodeByInode(ino_t inodeNumber);
  InotifyNode * findNodeByPath(const std::string path);
  std::string getParentPath(const std::string &filePath);
  bool existWatchedPath();

  std::string mError;
  const int mInotifyInstance;
  std::map<int, InotifyNode *> *mInotifyNodeByWatchDescriptor;
  std::map<ino_t, InotifyNode *> inodes;
  InotifyNode *mRoot;
  std::vector<std::string> mExcludedPaths;
  std::string mWatchedPath;

  friend class InotifyNode;
};

#endif
