#ifndef NSFW_FILE_DESCRIPTOR_H
#define NSFW_FILE_DESCRIPTOR_H

#include <unistd.h>

// Owns a descriptor and closes it once, on reset() or destruction: its number may belong to another file from then on,
// so whatever was handed it, see get(), has to be done with it by then.
class FileDescriptor {
public:
  explicit FileDescriptor(int descriptor): mDescriptor(descriptor) {}
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  ~FileDescriptor() { reset(); }

  int get() const { return mDescriptor; }
  bool isOpen() const { return mDescriptor != -1; }

  void reset() {
    if (mDescriptor != -1) {
      close(mDescriptor);
      mDescriptor = -1;
    }
  }

private:
  int mDescriptor;
};

#endif
