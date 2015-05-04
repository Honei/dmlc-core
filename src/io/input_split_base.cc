#define _CRT_SECURE_NO_WARNINGS
#include <algorithm>
#include <dmlc/logging.h>
#include "./line_split.h"

namespace dmlc {
namespace io {
void InputSplitBase::Init(FileSystem *filesys,
                          const char *uri,
                          unsigned rank,
                          unsigned nsplit,
                          size_t align_bytes) {
  this->filesys_ = filesys;
  // initialize the path
  this->InitInputFileInfo(uri);  
  file_offset_.resize(files_.size() + 1);
  file_offset_[0] = 0;
  for (size_t i = 0; i < files_.size(); ++i) {
    file_offset_[i + 1] = file_offset_[i] + files_[i].size;
    CHECK(files_[i].size % align_bytes == 0)
        << "file do not align by " << align_bytes << " bytes";
  }
  size_t ntotal = file_offset_.back();
  size_t nstep = (ntotal + nsplit - 1) / nsplit;
  // align the nstep to 4 bytes
  nstep = ((nstep + align_bytes - 1) / align_bytes) * align_bytes;
  offset_begin_ = std::min(nstep * rank, ntotal);
  offset_end_ = std::min(nstep * (rank + 1), ntotal);
  offset_curr_ = offset_begin_;
  if (offset_begin_ == offset_end_) return;
  file_ptr_ = std::upper_bound(file_offset_.begin(),
                               file_offset_.end(),
                               offset_begin_) - file_offset_.begin() - 1;
  file_ptr_end_ = std::upper_bound(file_offset_.begin(),
                                   file_offset_.end(),
                                   offset_end_) - file_offset_.begin() - 1;
  // find the exact ending position
  if (offset_end_ != file_offset_[file_ptr_end_]) {
    CHECK(offset_end_ >file_offset_[file_ptr_end_]);
    CHECK(file_ptr_end_ < files_.size());
    fs_ = filesys_->OpenForRead(files_[file_ptr_end_].path);
    fs_->Seek(offset_end_ - file_offset_[file_ptr_end_]);
    offset_end_ += SeekRecordBegin(fs_);
    delete fs_;
  }
  fs_ = filesys_->OpenForRead(files_[file_ptr_].path); 
  if (offset_begin_ != file_offset_[file_ptr_]) {
    fs_->Seek(offset_begin_ - file_offset_[file_ptr_]);
    offset_begin_ += SeekRecordBegin(fs_);
  }
  this->BeforeFirst();
}

void InputSplitBase::BeforeFirst(void) {
  if (offset_begin_ >= offset_end_) return;
  size_t fp = std::upper_bound(file_offset_.begin(),
                               file_offset_.end(),
                               offset_begin_) - file_offset_.begin() - 1;
  if (file_ptr_ != fp) {
    delete fs_;
    file_ptr_ = fp;
    fs_ = filesys_->OpenForRead(files_[file_ptr_].path);
  }
  // seek to beginning of stream
  fs_->Seek(offset_begin_ - file_offset_[file_ptr_]);
  offset_curr_ = offset_begin_;
  tmp_chunk_.begin = tmp_chunk_.end = NULL;
  // clear overflow buffer
  overflow_.clear();
}

InputSplitBase::~InputSplitBase(void) {
  if (fs_ != NULL) delete fs_;
  // no need to delete filesystem, it was singleton
}

void InputSplitBase::InitInputFileInfo(const char *uri) {
  // split by :
  const char *dlm = ";";
  std::string uri_ = uri;
  char *p = std::strtok(BeginPtr(uri_), dlm);
  std::vector<URI> vec;
  
  while (p != NULL) {
    URI path(p);
    FileInfo info = filesys_->GetPathInfo(path);
    if (info.type == kDirectory) {
      std::vector<FileInfo> dfiles;
      filesys_->ListDirectory(info.path, &dfiles);
      for (size_t i = 0; i < dfiles.size(); ++i) {
        if (dfiles[i].size != 0 && dfiles[i].type == kFile) {
          files_.push_back(dfiles[i]);
        }
      }
    } else {
      if (info.size != 0) {
        files_.push_back(info);
      }
    }
    p = std::strtok(NULL, dlm);
  }
}

size_t InputSplitBase::Read(void *ptr, size_t size) {
  if (offset_curr_ +  size > offset_end_) {
    size = offset_end_ - offset_curr_;
  }
  if (size == 0) return 0;
  size_t nleft = size;
  char *buf = reinterpret_cast<char*>(ptr);
  while (true) {
    size_t n = fs_->Read(buf, nleft);
    nleft -= n; buf += n;
    offset_curr_ += n;
    if (nleft == 0) break;
    if (offset_curr_ != file_offset_[file_ptr_ + 1]) {
      LOG(FATAL) << "FILE size not calculated correctly";
    }
    if (file_ptr_ + 1 >= files_.size()) break;
    file_ptr_ += 1;
    delete fs_;
    fs_ = filesys_->OpenForRead(files_[file_ptr_].path);    
  }
  return size - nleft;
}

bool InputSplitBase::ReadChunk(void *buf, size_t *size) {
  size_t max_size = *size;
  if (max_size <= overflow_.length()) {
    *size = 0; return true;
  }
  if (overflow_.length() != 0) { 
    std::memcpy(buf, BeginPtr(overflow_), overflow_.length());  
  }
  size_t olen = overflow_.length();
  overflow_.resize(0);
  size_t nread = this->Read(reinterpret_cast<char*>(buf) + olen,
                            max_size - olen);
  nread += olen;
  if (nread == 0) return false;
  if (nread != max_size) {
    *size = nread;
    return true;
  } else {
    const char *bptr = reinterpret_cast<const char*>(buf);
    // return the last position where a record starts
    const char *bend = this->FindLastRecordBegin(bptr, bptr + max_size);
    *size = bend - bptr;
    overflow_.resize(max_size - *size);
    if (overflow_.length() != 0) {
      std::memcpy(BeginPtr(overflow_), bend, overflow_.length());
    }
    return true;
  }
}

bool InputSplitBase::Chunk::Load(InputSplitBase *split, size_t buffer_size) {
  if (buffer_size + 1 > data.size()) {
    data.resize(buffer_size + 1);
  }
  while (true) {
    // leave one tail chunk
    size_t size = (data.size() - 1) * sizeof(size_t);
    // set back to 0 for string safety
    data.back() = 0;
    if (!split->ReadChunk(BeginPtr(data), &size)) return false;
    if (size == 0) {
      data.resize(data.size() * 2);
    } else {
      begin = reinterpret_cast<char *>(BeginPtr(data));
      end = begin + size;
      break;
    }
  }
  return true;
}

bool InputSplitBase::ExtractNextChunk(Blob *out_chunk, Chunk *chunk) {
  if (chunk->begin == chunk->end) return false;
  out_chunk->dptr = chunk->begin;
  out_chunk->size = chunk->end - chunk->begin;
  chunk->begin = chunk->end;  
  return true;
}
}  // namespace io
}  // namespace dmlc
