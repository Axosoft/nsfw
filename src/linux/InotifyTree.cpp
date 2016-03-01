#include "../../includes/linux/InotifyTree.h"
/**
 * InotifyTree ---------------------------------------------------------------------------------------------------------
 */
InotifyTree::InotifyTree(int inotifyInstance, std::string path):
  mInotifyInstance(inotifyInstance) {
  mInotifyNodeByWatchDescriptor = new std::map<int, InotifyNode *>;

  std::string directory;
  std::string watchName;
  if (path.length() == 1 && path[0] == '/') {
    directory = "";
    watchName = "";
  } else {
    uint32_t location = path.find_last_of("/");
    directory = (location == 0) ? "" : path.substr(0, location);
    watchName = path.substr(location + 1);
  }

  mRoot = new InotifyNode(
    mInotifyNodeByWatchDescriptor,
    mInotifyInstance,
    NULL,
    directory,
    watchName
  );

  if (
    !mRoot->isAlive() ||
    !mRoot->inotifyInit()
  ) {
    delete mRoot;
    mRoot = NULL;
    return;
  }
}

void InotifyTree::addDirectory(int wd, std::string name) {
  auto nodeIterator = mInotifyNodeByWatchDescriptor->find(wd);
  if (nodeIterator == mInotifyNodeByWatchDescriptor->end()) {
    return;
  }

  InotifyNode *node = nodeIterator->second;
  node->addChild(name);
}

bool InotifyTree::getPath(std::string &out, int wd) {
  auto nodeIterator = mInotifyNodeByWatchDescriptor->find(wd);
  if (nodeIterator == mInotifyNodeByWatchDescriptor->end()) {
    return false;
  }

  out = nodeIterator->second->getFullPath();
  return true;
}

bool InotifyTree::isRootAlive() {
  return mRoot != NULL;
}

void InotifyTree::removeDirectory(int wd) {
  auto nodeIterator = mInotifyNodeByWatchDescriptor->find(wd);
  if (nodeIterator == mInotifyNodeByWatchDescriptor->end()) {
    return;
  }

  InotifyNode *node = nodeIterator->second;
  InotifyNode *parent = node->getParent();
  if (parent == NULL) {
    delete mRoot;
    mRoot = NULL;
    return;
  }

  parent->removeChild(node->getName());
}

void InotifyTree::renameDirectory(int wd, std::string oldName, std::string newName) {
  auto nodeIterator = mInotifyNodeByWatchDescriptor->find(wd);
  if (nodeIterator == mInotifyNodeByWatchDescriptor->end()) {
    return;
  }

  nodeIterator->second->renameChild(oldName, newName);
}

InotifyTree::~InotifyTree() {
  if (isRootAlive()) {
    delete mRoot;
  }
}

/**
 * InotifyNode ---------------------------------------------------------------------------------------------------------
 */
InotifyTree::InotifyNode::InotifyNode(
  std::map<int, InotifyNode *> *inotifyNodeByWatchDescriptor,
  int inotifyInstance,
  InotifyNode *parent,
  std::string directory,
  std::string name
):
  mDirectory(directory),
  mInotifyInstance(inotifyInstance),
  mInotifyNodeByWatchDescriptor(inotifyNodeByWatchDescriptor),
  mName(name),
  mParent(parent) {
  mChildren = new std::map<std::string, InotifyNode *>;
  mFullPath = createFullPath(mDirectory, mName);
  mWatchDescriptorInitialized = false;

  dirent ** directoryContents = NULL;

  int resultCountOrError = scandir(
    mFullPath.c_str(),
    &directoryContents,
    NULL,
    alphasort
  );

  mAlive = (resultCountOrError >= 0);

  if (!mAlive) {
    return;
  }

  for (uint32_t i = 0; i < resultCountOrError; ++i) {
    std::string fileName = directoryContents[i]->d_name;

    if (
      fileName == "." ||
      fileName == ".."
    ) {
      continue;
    }

    std::string filePath = createFullPath(mFullPath, fileName);

    struct stat file;

    if (
      stat(filePath.c_str(), &file) < 0 ||
      !S_ISDIR(file.st_mode)
    ) {
      continue;
    }

    InotifyNode *child = new InotifyNode(
      mInotifyNodeByWatchDescriptor,
      mInotifyInstance,
      this,
      mFullPath,
      fileName
    );

    if (child->isAlive()) {
      (*mChildren)[fileName] = child;
    } else {
      delete child;
    }
  }

  for (uint32_t i = 0; i < resultCountOrError; ++i) {
    delete directoryContents[i];
  }

  delete[] directoryContents;
}

InotifyTree::InotifyNode::~InotifyNode() {
  if (mWatchDescriptorInitialized) {
    inotify_rm_watch(mInotifyInstance, mWatchDescriptor);
    auto nodeIterator = mInotifyNodeByWatchDescriptor->find(mWatchDescriptor);
    if (nodeIterator != mInotifyNodeByWatchDescriptor->end()) {
      mInotifyNodeByWatchDescriptor->erase(nodeIterator);
    }
  }

  for (auto i = mChildren->begin(); i != mChildren->end(); ++i) {
    delete i->second;
    i->second = NULL;
  }
  delete mChildren;
}

void InotifyTree::InotifyNode::addChild(std::string name) {
  InotifyNode *child = new InotifyNode(
    mInotifyNodeByWatchDescriptor,
    mInotifyInstance,
    this,
    mFullPath,
    name
  );

  if (
    child->isAlive() &&
    child->inotifyInit()
  ) {
    (*mChildren)[name] = child;
  } else {
    delete child;
  }
}

void InotifyTree::InotifyNode::fixPaths() {
  std::string parentPath = mParent->getFullPath();
  std::string fullPath = createFullPath(parentPath, mName);

  if (fullPath == mFullPath) {
    return;
  }

  mDirectory = parentPath;
  mFullPath = fullPath;

  for(auto i = mChildren->begin(); i != mChildren->end(); ++i) {
    i->second->fixPaths();
  }
}

std::string InotifyTree::InotifyNode::getFullPath() {
  return mFullPath;
}

std::string InotifyTree::InotifyNode::getName() {
  return mName;
}

bool InotifyTree::InotifyNode::isAlive() {
  return mAlive;
}

InotifyTree::InotifyNode *InotifyTree::InotifyNode::getParent() {
  return mParent;
}

bool InotifyTree::InotifyNode::inotifyInit() {
  // TODO: error handling
  mWatchDescriptor = inotify_add_watch(
    mInotifyInstance,
    mFullPath.c_str(),
    ATTRIBUTES
  );

  mAlive = (mWatchDescriptor != -1);

  if (mAlive) {
    auto *childrenToRemove = new std::vector<std::string>;
    childrenToRemove->reserve(mChildren->size());
    for (auto i = mChildren->begin(); i != mChildren->end(); ++i) {
      if (!i->second->inotifyInit()) {
        childrenToRemove->push_back(i->second->getName());
      }
    }

    if (childrenToRemove->size() > 0) {
      struct stat file;

      if (
        stat(mFullPath.c_str(), &file) < 0 ||
        !S_ISDIR(file.st_mode)
      ) {
        mAlive = false;
      } else {
        for (auto i = childrenToRemove->begin(); i != childrenToRemove->end(); ++i) {
          removeChild(*i);
        }
        (*mInotifyNodeByWatchDescriptor)[mWatchDescriptor] = this;
      }
    } else {
      (*mInotifyNodeByWatchDescriptor)[mWatchDescriptor] = this;
    }
    delete childrenToRemove;
  }
}

void InotifyTree::InotifyNode::removeChild(std::string name) {
  auto child = mChildren->find(name);
  if (child != mChildren->end()) {
    delete child->second;
    child->second = NULL;
    mChildren->erase(child);
  }
}

void InotifyTree::InotifyNode::renameChild(std::string oldName, std::string newName) {
  auto child = mChildren->find(oldName);
  if (child == mChildren->end()) {
    child = mChildren->find(newName);
    if (child == mChildren->end()) {
      addChild(newName);
    }
    return;
  }

  InotifyNode *node = child->second;
  mChildren->erase(child);
  node->setName(newName);
  (*mChildren)[newName] = node;
}

void InotifyTree::InotifyNode::setName(std::string name) {
  mName = name;
  fixPaths();
}

std::string InotifyTree::InotifyNode::createFullPath(std::string parentPath, std::string name) {
  std::stringstream fullPathStream;

  fullPathStream
    << parentPath
    << '/'
    << name;

  return fullPathStream.str();
}
