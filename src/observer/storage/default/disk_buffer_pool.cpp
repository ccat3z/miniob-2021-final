/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Meiyi & Longda on 2021/4/13.
//
#include "disk_buffer_pool.h"
#include <errno.h>
#include <string.h>

#include "common/lang/mutex.h"
#include "common/log/log.h"
#include "common/os/os.h"

using namespace common;

int DiskBufferPool::POOL_NUM = MAX_OPEN_FILE / 4;

unsigned long current_time()
{
  struct timespec tp;
  clock_gettime(CLOCK_MONOTONIC, &tp);
  return tp.tv_sec * 1000 * 1000 * 1000UL + tp.tv_nsec;
}

BPFileHandle::BPFileHandle()
{
  memset((void *)this, 0, sizeof(*this));
}

BPFileHandle::~BPFileHandle()
{
  if (file_name != nullptr) {
    ::free((void *)file_name);
    file_name = nullptr;
  }
}

BPManager::BPManager(const char *name) : MemPoolSimple<Frame>(name)
{}

static bool match_purge(void *item, void *arg)
{
  Frame *frame = (Frame *)item;
  return frame->can_purge();
}

Frame *BPManager::begin_purge()
{
  return MemPoolSimple<Frame>::find(match_purge, nullptr);
}

struct MatchFilePage {
  MatchFilePage(int file_desc, PageNum page_num)
  {
    this->file_desc = file_desc;
    this->page_num = page_num;
  }
  int file_desc;
  PageNum page_num;
};

static bool match_file_page(void *item, void *arg)
{
  Frame *frame = (Frame *)item;
  MatchFilePage *file_page = (MatchFilePage *)arg;

  if (frame->file_desc == file_page->file_desc && frame->page.page_num == file_page->page_num)
    return true;

  return false;
}

Frame *BPManager::get(int file_desc, PageNum page_num)
{
  MatchFilePage file_page(file_desc, page_num);
  return MemPoolSimple<Frame>::find(match_file_page, &file_page);
}

static bool match_file(void *item, void *arg)
{
  Frame *frame = (Frame *)item;
  int *file_desc = (int *)arg;

  if (frame->file_desc == *file_desc)
    return true;

  return false;
}

std::list<Frame *> BPManager::find_list(int file_desc)
{
  return find_all(match_file, &file_desc);
}

DiskBufferPool *theGlobalDiskBufferPool()
{
  static DiskBufferPool *instance = DiskBufferPool::mk_instance();

  return instance;
}

DiskBufferPool::DiskBufferPool() : bp_manager_("BPManager")
{
  bp_manager_.init(false, DiskBufferPool::POOL_NUM, BP_BUFFER_SIZE);
};

DiskBufferPool::~DiskBufferPool()
{
  for (int i = 0; i < MAX_OPEN_FILE; i++) {
    BPFileHandle *file_handle = open_list_[i];
    if (file_handle == nullptr) {
      continue;
    }

    close_file(i);
    open_list_[i] = nullptr;
  }

  bp_manager_.cleanup();
  LOG_INFO("Exit");
}

RC DiskBufferPool::create_file(const char *file_name)
{
  int fd = open(file_name, O_RDWR | O_CREAT | O_EXCL, S_IREAD | S_IWRITE);
  if (fd < 0) {
    LOG_ERROR("Failed to create %s, due to %s.", file_name, strerror(errno));
    return RC::SCHEMA_DB_EXIST;
  }

  close(fd);

  /**
   * Here don't care about the failure
   */
  fd = open(file_name, O_RDWR);
  if (fd < 0) {
    LOG_ERROR("Failed to open for readwrite %s, due to %s.", file_name, strerror(errno));
    return RC::IOERR_ACCESS;
  }

  Page page;
  memset(&page, 0, sizeof(Page));

  BPFileSubHeader *fileSubHeader;
  fileSubHeader = (BPFileSubHeader *)page.data;
  fileSubHeader->allocated_pages = 1;
  fileSubHeader->page_count = 1;

  char *bitmap = page.data + (int)BP_FILE_SUB_HDR_SIZE;
  bitmap[0] |= 0x01;
  if (lseek(fd, 0, SEEK_SET) == -1) {
    LOG_ERROR("Failed to seek file %s to position 0, due to %s .", file_name, strerror(errno));
    close(fd);
    return RC::IOERR_SEEK;
  }

  if (write(fd, (char *)&page, sizeof(Page)) != sizeof(Page)) {
    LOG_ERROR("Failed to write header to file %s, due to %s.", file_name, strerror(errno));
    close(fd);
    return RC::IOERR_WRITE;
  }

  close(fd);
  LOG_INFO("Successfully create %s.", file_name);
  return RC::SUCCESS;
}

RC DiskBufferPool::open_file(const char *file_name, int *file_id, bool compressed)
{
  int fd, i, size = 0, empty_id = -1;
  // This part isn't gentle, the better method is using LRU queue.
  for (i = 0; i < MAX_OPEN_FILE; i++) {
    if (open_list_[i]) {
      size++;
      if (!strcmp(open_list_[i]->file_name, file_name)) {
        *file_id = i;
        LOG_INFO("%s has already been opened.", file_name);
        return RC::SUCCESS;
      }
    } else if (empty_id == -1) {
      empty_id = i;
    }
  }

  i = size;
  if (i >= MAX_OPEN_FILE && open_list_[i - 1]) {
    LOG_ERROR("Failed to open file %s, because too much files have been opened.", file_name);
    return RC::BUFFERPOOL_OPEN_TOO_MANY_FILES;
  }

  if ((fd = open(file_name, O_RDWR)) < 0) {
    LOG_ERROR("Failed to open file %s, because %s.", file_name, strerror(errno));
    return RC::IOERR_ACCESS;
  }
  LOG_INFO("Successfully open file %s.", file_name);

  BPFileHandle *file_handle = new (std::nothrow) BPFileHandle();
  if (file_handle == nullptr) {
    LOG_ERROR("Failed to alloc memory of BPFileHandle for %s.", file_name);
    close(fd);
    return RC::NOMEM;
  }

  RC tmp = RC::SUCCESS;
  file_handle->bopen = true;
  file_handle->file_name = strdup(file_name);
  file_handle->file_desc = fd;
  file_handle->compressed = compressed;
  if ((tmp = allocate_page(&file_handle->hdr_frame)) != RC::SUCCESS) {
    LOG_ERROR("Failed to allocate block for %s's BPFileHandle.", file_name);
    delete file_handle;
    close(fd);
    return tmp;
  }
  file_handle->hdr_frame->dirty = false;
  file_handle->hdr_frame->file_desc = fd;
  file_handle->hdr_frame->pin_count = 1;
  file_handle->hdr_frame->acc_time = current_time();
  file_handle->hdr_frame->file_id = empty_id;
  if ((tmp = load_page(0, file_handle, file_handle->hdr_frame)) != RC::SUCCESS) {
    LOG_ERROR("Failed to load first page of %s, due to %s.", file_name, strerror(errno));
    file_handle->hdr_frame->pin_count = 0;
    purge_page(file_handle->hdr_frame);
    close(fd);
    delete file_handle;
    return tmp;
  }

  file_handle->hdr_page = &(file_handle->hdr_frame->page);
  file_handle->bitmap = file_handle->hdr_page->data + BP_FILE_SUB_HDR_SIZE;
  file_handle->file_sub_header = (BPFileSubHeader *)file_handle->hdr_page->data;
  open_list_[empty_id] = file_handle;
  *file_id = empty_id;

  LOG_INFO("Successfully open %s. file_id=%d, hdr_frame=%p", file_name, *file_id, file_handle->hdr_frame);
  return RC::SUCCESS;
}

RC DiskBufferPool::close_file(int file_id)
{
  RC tmp;
  if ((tmp = check_file_id(file_id)) != RC::SUCCESS) {
    LOG_ERROR("Failed to close file, due to invalid fileId %d", file_id);
    return tmp;
  }

  BPFileHandle *file_handle = open_list_[file_id];
  file_handle->hdr_frame->pin_count--;
  if ((tmp = purge_all_pages(file_handle)) != RC::SUCCESS) {
    file_handle->hdr_frame->pin_count++;
    LOG_ERROR("Failed to close file %d:%s, due to failed to purge all pages.", file_id, file_handle->file_name);
    return tmp;
  }

  disposed_pages.erase(file_handle->file_desc);

  if (close(file_handle->file_desc) < 0) {
    LOG_ERROR("Failed to close fileId:%d, fileName:%s, error:%s", file_id, file_handle->file_name, strerror(errno));
    return RC::IOERR_CLOSE;
  }
  open_list_[file_id] = nullptr;
  LOG_INFO("Successfully close file %d:%s.", file_id, file_handle->file_name);
  delete file_handle;
  return RC::SUCCESS;
}

RC DiskBufferPool::get_this_page(int file_id, PageNum page_num, BPPageHandle *page_handle)
{
  RC tmp;
  if ((tmp = check_file_id(file_id)) != RC::SUCCESS) {
    LOG_ERROR("Failed to load page %d, due to invalid fileId %d", page_num, file_id);
    return tmp;
  }

  BPFileHandle *file_handle = open_list_[file_id];
  if ((tmp = check_page_num(page_num, file_handle)) != RC::SUCCESS) {
    LOG_ERROR("Failed to load page %s:%d, due to invalid pageNum.", file_handle->file_name, page_num);
    return tmp;
  }

  Frame *used_match_frame = bp_manager_.get(file_handle->file_desc, page_num);
  if (used_match_frame != nullptr) {
    page_handle->frame = used_match_frame;
    page_handle->frame->pin_count++;
    page_handle->frame->acc_time = current_time();
    page_handle->open = true;

    bp_manager_.mark_modified(used_match_frame);

    return RC::SUCCESS;
  }

  // Allocate one page and load the data into this page
  if ((tmp = allocate_page(&(page_handle->frame))) != RC::SUCCESS) {
    LOG_ERROR("Failed to load page %s:%d, due to failed to alloc page.", file_handle->file_name, page_num);
    return tmp;
  }
  page_handle->frame->dirty = false;
  page_handle->frame->file_desc = file_handle->file_desc;
  page_handle->frame->pin_count = 1;
  page_handle->frame->acc_time = current_time();
  page_handle->frame->file_id = file_id;
  if ((tmp = load_page(page_num, file_handle, page_handle->frame)) != RC::SUCCESS) {
    LOG_ERROR("Failed to load page %s:%d", file_handle->file_name, page_num);
    page_handle->frame->pin_count = 0;
    purge_page(page_handle->frame);
    return tmp;
  }

  page_handle->open = true;
  return RC::SUCCESS;
}

RC DiskBufferPool::allocate_page(int file_id, BPPageHandle *page_handle)
{
  RC tmp;
  if ((tmp = check_file_id(file_id)) != RC::SUCCESS) {
    LOG_ERROR("Failed to alloc page, due to invalid fileId %d", file_id);
    return tmp;
  }

  BPFileHandle *file_handle = open_list_[file_id];

  int byte = 0, bit = 0;
  if ((file_handle->file_sub_header->allocated_pages) < (file_handle->file_sub_header->page_count)) {
    // There is one free page
    for (int i = 0; i < file_handle->file_sub_header->page_count; i++) {
      byte = i / 8;
      bit = i % 8;
      if (((file_handle->bitmap[byte]) & (1 << bit)) == 0) {
        (file_handle->file_sub_header->allocated_pages)++;
        file_handle->bitmap[byte] |= (1 << bit);
        // TODO,  do we need clean the loaded page's data?
        return get_this_page(file_id, i, page_handle);
      }
    }
  }

  if ((tmp = allocate_page(&(page_handle->frame))) != RC::SUCCESS) {
    LOG_ERROR("Failed to allocate page %s, due to no free page.", file_handle->file_name);
    return tmp;
  }

  PageNum page_num = file_handle->file_sub_header->page_count;
  file_handle->file_sub_header->allocated_pages++;
  file_handle->file_sub_header->page_count++;

  byte = page_num / 8;
  bit = page_num % 8;
  file_handle->bitmap[byte] |= (1 << bit);
  file_handle->hdr_frame->dirty = true;

  page_handle->frame->dirty = false;
  page_handle->frame->file_desc = file_handle->file_desc;
  page_handle->frame->pin_count = 1;
  page_handle->frame->acc_time = current_time();
  page_handle->frame->file_id = file_id;
  memset(&(page_handle->frame->page), 0, sizeof(Page));
  page_handle->frame->page.page_num = file_handle->file_sub_header->page_count - 1;

  // Use flush operation to extension file
  if ((tmp = flush_page(page_handle->frame)) != RC::SUCCESS) {
    LOG_WARN("Failed to alloc page %s , due to failed to extend one page.", file_handle->file_name);
    // skip return false, delay flush the extended page
    // return tmp;
  }

  page_handle->open = true;
  return RC::SUCCESS;
}

RC DiskBufferPool::get_page_num(BPPageHandle *page_handle, PageNum *page_num)
{
  if (!page_handle->open)
    return RC::BUFFERPOOL_CLOSED;
  *page_num = page_handle->frame->page.page_num;
  return RC::SUCCESS;
}

RC DiskBufferPool::get_data(BPPageHandle *page_handle, char **data)
{
  if (!page_handle->open)
    return RC::BUFFERPOOL_CLOSED;
  *data = page_handle->frame->page.data;
  return RC::SUCCESS;
}

RC DiskBufferPool::mark_dirty(BPPageHandle *page_handle)
{
  page_handle->frame->dirty = true;
  return RC::SUCCESS;
}

RC DiskBufferPool::unpin_page(BPPageHandle *page_handle)
{
  page_handle->open = false;
  if (--page_handle->frame->pin_count == 0) {
    int file_desc = page_handle->frame->file_desc;
    auto it = disposed_pages.find(file_desc);
    if (it != disposed_pages.end()) {
      BPDisposedPages &disposed_page = it->second;
      PageNum page_num = page_handle->frame->page.page_num;
      auto pages_it = disposed_page.pages.find(page_num);
      if (pages_it != disposed_page.pages.end()) {
        LOG_INFO("Dispose file_id:%d, page:%d", disposed_page.file_id, page_num);
        dispose_page(disposed_page.file_id, page_num);
        disposed_page.pages.erase(pages_it);
      }
    }
  }

  return RC::SUCCESS;
}

/**
 * dispose_page will delete the data of the page of pageNum, free the page both from buffer pool and data file.
 * purge_page will purge the page of pageNum, free the page from buffer pool
 * @param fileID
 * @param pageNum
 * @return
 */
RC DiskBufferPool::dispose_page(int file_id, PageNum page_num)
{
  RC rc;
  if ((rc = check_file_id(file_id)) != RC::SUCCESS) {
    LOG_ERROR("Failed to alloc page, due to invalid fileId %d", file_id);
    return rc;
  }

  BPFileHandle *file_handle = open_list_[file_id];
  if ((rc = check_page_num(page_num, file_handle)) != RC::SUCCESS) {
    LOG_ERROR("Failed to dispose page %s:%d, due to invalid pageNum", file_handle->file_name, page_num);
    return rc;
  }

  rc = purge_page(file_handle, page_num);
  if (rc != RC::SUCCESS) {
    LOG_INFO("Dispose page %s:%d later, due to this page is being used", file_handle->file_name, page_num);

    auto it = disposed_pages.find(file_handle->file_desc);
    if (it == disposed_pages.end()) {
      BPDisposedPages disposed_page;
      disposed_page.file_id = file_id;
      disposed_page.pages.insert(page_num);
      disposed_pages.insert(std::pair<int, BPDisposedPages>(file_handle->file_desc, disposed_page));
    } else {
      BPDisposedPages &disposed_page = it->second;
      disposed_page.pages.insert(page_num);
    }

    return rc;
  }

  file_handle->hdr_frame->dirty = true;
  file_handle->file_sub_header->allocated_pages--;
  // file_handle->pFileSubHeader->pageCount--;
  char tmp = 1 << (page_num % 8);
  file_handle->bitmap[page_num / 8] &= ~tmp;
  return RC::SUCCESS;
}

RC DiskBufferPool::purge_page(int file_id, PageNum page_num)
{
  RC rc;
  if ((rc = check_file_id(file_id)) != RC::SUCCESS) {
    LOG_ERROR("Failed to alloc page, due to invalid fileId %d", file_id);
    return rc;
  }
  BPFileHandle *file_handle = open_list_[file_id];
  return purge_page(file_handle, page_num);
}

RC DiskBufferPool::purge_page(Frame *buf)
{
  if (buf->pin_count > 0) {
    LOG_INFO("Begin to free page %d of %d, but it's pinned, pin_count:%d.",
        buf->page.page_num,
        buf->file_desc,
        buf->pin_count);
    return RC::LOCKED_UNLOCK;
  }

  if (buf->dirty) {
    RC rc = flush_page(buf);
    if (rc != RC::SUCCESS) {
      LOG_WARN("Failed to flush page %d of %d during purge page.", buf->page.page_num, buf->file_desc);
      return rc;
    }
  }

  LOG_DEBUG("Successfully purge frame =%p, page %d of %d", buf, buf->page.page_num, buf->file_desc);
  bp_manager_.free(buf);
  return RC::SUCCESS;
}

/**
 * dispose_page will delete the data of the page of pageNum
 * force_page will flush the page of pageNum
 * @param fileHandle
 * @param pageNum
 * @return
 */
RC DiskBufferPool::purge_page(BPFileHandle *file_handle, PageNum page_num)
{
  Frame *used_frame = bp_manager_.get(file_handle->file_desc, page_num);
  if (used_frame != nullptr) {
    return purge_page(used_frame);
  }

  return RC::SUCCESS;
}

RC DiskBufferPool::purge_all_pages(int file_id)
{
  RC rc = check_file_id(file_id);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to flush pages due to invalid file_id %d", file_id);
    return rc;
  }

  BPFileHandle *file_handle = open_list_[file_id];
  return purge_all_pages(file_handle);
}

RC DiskBufferPool::purge_all_pages(BPFileHandle *file_handle)
{
  std::list<Frame *> used = bp_manager_.find_list(file_handle->file_desc);
  for (std::list<Frame *>::iterator it = used.begin(); it != used.end(); ++it) {
    Frame *frame = *it;
    // Force flush
    // if (frame->pin_count > 0) {
    //   LOG_WARN("The page has been pinned, file_id:%d, pagenum:%d", frame->file_desc, frame->page.page_num);
    //   continue;
    // }
    if (frame->dirty) {
      RC rc = flush_page(frame);
      if (rc != RC::SUCCESS) {
        LOG_ERROR("Failed to flush all pages' of %s.", file_handle->file_name);
        return rc;
      }
    }
    bp_manager_.free(frame);
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::flush_page(Frame *frame)
{
  if (frame->page.page_num < UNCOMPRESSED_PAGE_NUM || !open_list_[frame->file_id]->compressed) {
    return flush_uncompressed_page(frame);
  }

  s64_t offset = UNCOMPRESSED_PAGE_NUM * sizeof(Page) +
                 ((s64_t)frame->page.page_num - UNCOMPRESSED_PAGE_NUM) * sizeof(CompressedPage);
  CompressedPage comp_page;
  compress_page(&frame->page, &comp_page, !frame->dirty);

  if (lseek(frame->file_desc, offset, SEEK_SET) == offset - 1) {
    LOG_ERROR("Failed to flush page %lld of %d due to failed to seek %s.", offset, frame->file_desc, strerror(errno));
    return RC::IOERR_SEEK;
  }

  if (write(frame->file_desc, &comp_page, sizeof(CompressedPage)) != sizeof(CompressedPage)) {
    LOG_ERROR("Failed to flush page %lld of %d due to %s.", offset, frame->file_desc, strerror(errno));
    return RC::IOERR_WRITE;
  }
  frame->dirty = false;
  LOG_DEBUG("Flush block. file desc=%d, page num=%d", frame->file_desc, frame->page.page_num);

  return RC::SUCCESS;
}

RC DiskBufferPool::flush_uncompressed_page(Frame *frame)
{
  // The better way is use mmap the block into memory,
  // so it is easier to flush data to file.

  s64_t offset = ((s64_t)frame->page.page_num) * sizeof(Page);
  if (lseek(frame->file_desc, offset, SEEK_SET) == offset - 1) {
    LOG_ERROR("Failed to flush page %lld of %d due to failed to seek %s.", offset, frame->file_desc, strerror(errno));
    return RC::IOERR_SEEK;
  }

  if (write(frame->file_desc, &(frame->page), sizeof(Page)) != sizeof(Page)) {
    LOG_ERROR("Failed to flush page %lld of %d due to %s.", offset, frame->file_desc, strerror(errno));
    return RC::IOERR_WRITE;
  }
  frame->dirty = false;
  LOG_DEBUG("Flush block. file desc=%d, page num=%d", frame->file_desc, frame->page.page_num);

  return RC::SUCCESS;
}

RC DiskBufferPool::allocate_page(Frame **buffer)
{
  Frame *frame = bp_manager_.alloc();
  if (frame != nullptr) {
    *buffer = frame;
    return RC::SUCCESS;
  }

  frame = bp_manager_.begin_purge();
  if (frame == nullptr) {
    LOG_ERROR("All pages have been used and pinned.");
    return RC::NOMEM;
  }

  if (frame->dirty) {
    RC rc = flush_page(frame);
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to aclloc block due to failed to flush old block.");
      return rc;
    }
  }

  bp_manager_.mark_modified(frame);

  *buffer = frame;
  return RC::SUCCESS;
}

RC DiskBufferPool::check_file_id(int file_id)
{
  if (file_id < 0 || file_id >= MAX_OPEN_FILE) {
    LOG_ERROR("Invalid fileId:%d.", file_id);
    return RC::BUFFERPOOL_ILLEGAL_FILE_ID;
  }
  if (!open_list_[file_id]) {
    LOG_ERROR("Invalid fileId:%d, it is empty.", file_id);
    return RC::BUFFERPOOL_ILLEGAL_FILE_ID;
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::get_page_count(int file_id, int *page_count)
{
  RC rc = RC::SUCCESS;
  if ((rc = check_file_id(file_id)) != RC::SUCCESS) {
    return rc;
  }
  *page_count = open_list_[file_id]->file_sub_header->page_count;
  return RC::SUCCESS;
}

RC DiskBufferPool::check_page_num(PageNum page_num, BPFileHandle *file_handle)
{
  if (page_num >= file_handle->file_sub_header->page_count) {
    LOG_ERROR("Invalid pageNum:%d, file's name:%s", page_num, file_handle->file_name);
    return RC::BUFFERPOOL_INVALID_PAGE_NUM;
  }
  if ((file_handle->bitmap[page_num / 8] & (1 << (page_num % 8))) == 0) {
    LOG_ERROR("Invalid pageNum:%d, file's name:%s", page_num, file_handle->file_name);
    return RC::BUFFERPOOL_INVALID_PAGE_NUM;
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::load_page(PageNum page_num, BPFileHandle *file_handle, Frame *frame)
{
  if (page_num < UNCOMPRESSED_PAGE_NUM || !file_handle->compressed) {
    return load_uncompressed_page(page_num, file_handle, frame);
  }

  s64_t offset =
      UNCOMPRESSED_PAGE_NUM * sizeof(Page) + ((s64_t)page_num - UNCOMPRESSED_PAGE_NUM) * sizeof(CompressedPage);
  CompressedPage comp_page;
  if (lseek(file_handle->file_desc, offset, SEEK_SET) == -1) {
    LOG_ERROR(
        "Failed to load page %s:%d, due to failed to lseek:%s.", file_handle->file_name, page_num, strerror(errno));

    return RC::IOERR_SEEK;
  }
  if (read(file_handle->file_desc, &comp_page, sizeof(CompressedPage)) != sizeof(CompressedPage)) {
    LOG_ERROR(
        "Failed to load page %s:%d, due to failed to read data:%s.", file_handle->file_name, page_num, strerror(errno));
    return RC::IOERR_READ;
  }
  return decompress_page(&frame->page, &comp_page);
}

RC DiskBufferPool::load_uncompressed_page(PageNum page_num, BPFileHandle *file_handle, Frame *frame)
{
  s64_t offset = ((s64_t)page_num) * sizeof(Page);
  if (lseek(file_handle->file_desc, offset, SEEK_SET) == -1) {
    LOG_ERROR(
        "Failed to load page %s:%d, due to failed to lseek:%s.", file_handle->file_name, page_num, strerror(errno));

    return RC::IOERR_SEEK;
  }
  if (read(file_handle->file_desc, &(frame->page), sizeof(Page)) != sizeof(Page)) {
    LOG_ERROR(
        "Failed to load page %s:%d, due to failed to read data:%s.", file_handle->file_name, page_num, strerror(errno));
    return RC::IOERR_READ;
  }
  return RC::SUCCESS;
}

inline void encode_9vischar(uint64_t &v, char *a, char *b, char *c)
{
  v = 0;
  for (int i = 2; i >= 0; --i) {
    v *= 96;
    if (c[i] == 0) {
      v += 95;
    } else {
      v += c[i] - ' ';
    }
  }
  for (int i = 2; i >= 0; --i) {
    v *= 96;
    if (b[i] == 0) {
      v += 95;
    } else {
      v += b[i] - ' ';
    }
  }
  for (int i = 2; i >= 0; --i) {
    v *= 96;
    if (a[i] == 0) {
      v += 95;
    } else {
      v += a[i] - ' ';
    }
  }
}

inline void decode_9vischar(uint64_t &v, char *a, char *b, char *c)
{
  for (int i = 0; i < 3; ++i) {
    a[i] = (v % 96) + ' ';
    if (a[i] == 127)
      a[i] = 0;
    v /= 96;
  }
  a[3] = 0;
  for (int i = 0; i < 3; ++i) {
    b[i] = (v % 96) + ' ';
    if (b[i] == 127)
      b[i] = 0;
    v /= 96;
  }
  b[3] = 0;
  for (int i = 0; i < 3; ++i) {
    c[i] = (v % 96) + ' ';
    if (c[i] == 127)
      c[i] = 0;
    v /= 96;
  }
  c[3] = 0;
}

// NOTE
// ??????compress_page???decompress_page?????????flush?????????,
// ?????????????????????????????????????????????, ??????create index????????????????????????sync.
// ???commit 327457a
//
// ??????, ??????????????????, ??????????????????????????????schema

RC DiskBufferPool::compress_page(Page *page, CompressedPage *comp_page, bool only_meta)
{
  comp_page->page_num = page->page_num;
  memcpy(comp_page->data, page->data, PAGE_FIRST_BLOCK_OFFSET);
  const int blocks_cap = (BP_PAGE_DATA_SIZE - PAGE_FIRST_BLOCK_OFFSET) / PAGE_BLOCK_SIZE;

  char *block = page->data + PAGE_FIRST_BLOCK_OFFSET;
  char *comp_block = comp_page->data + PAGE_FIRST_BLOCK_OFFSET;
  int last_c2;

  for (int idx = 0; idx < blocks_cap; block += PAGE_BLOCK_SIZE, comp_block += PAGE_COMPRESSED_BLOCK_SIZE, ++idx) {
    if (only_meta) {
      memcpy(comp_block, block, PAGE_COMPRESSED_BLOCK_SIZE);
    } else {
      UncompressedBlock *data = (UncompressedBlock *)block;
      CompressedBlock *comp_data = (CompressedBlock *)comp_block;

      // __trx
      comp_data->__trx == data->__trx;

      // c1: same as idx
      if (idx == 0) {
        comp_page->meta.delta_start = data->c1;
      }

      // c2: delta
      if (idx == 0) {
        comp_page->meta.c2_start = data->c2;
        last_c2 = data->c2;
      } else {
        if (comp_data->inc_c2 = (data->c2 != last_c2)) {
          ++last_c2;
        }
      }

      // c3
      comp_data->c3_high = data->c3 / (1 << 16);
      comp_data->c3_low = data->c3 % (1 << 16);

      // v1
      memcpy(comp_data->v1, data->v1, sizeof(comp_data->v1));

      // v2: AA
      comp_data->v2 = (data->v2[0] - 'A') * 26 + (data->v2[1] - 'A');

      // v3 - v5
      encode_9vischar(comp_data->v345, data->v3, data->v4, data->v5);

      // v6
      memcpy(comp_data->v6, data->v6, sizeof(comp_data->v6));

      // v7 - v9
      encode_9vischar(comp_data->v789, data->v7, data->v8, data->v9);
    }
  }
  return RC::SUCCESS;
}

RC DiskBufferPool::decompress_page(Page *page, CompressedPage *comp_page)
{
  page->page_num = comp_page->page_num;
  memcpy(page->data, comp_page->data, PAGE_FIRST_BLOCK_OFFSET);
  const int blocks_cap = (BP_PAGE_DATA_SIZE - PAGE_FIRST_BLOCK_OFFSET) / PAGE_BLOCK_SIZE;

  char *block = page->data + PAGE_FIRST_BLOCK_OFFSET;
  char *comp_block = comp_page->data + PAGE_FIRST_BLOCK_OFFSET;
  int last_c2;

  for (int idx = 0; idx < blocks_cap; block += PAGE_BLOCK_SIZE, comp_block += PAGE_COMPRESSED_BLOCK_SIZE, ++idx) {
    UncompressedBlock *data = (UncompressedBlock *)block;
    CompressedBlock *comp_data = (CompressedBlock *)comp_block;

    // __trx
    data->__trx == comp_data->__trx;

    // c1: same as idx
    data->c1 = comp_page->meta.delta_start + idx;

    // c2: delta
    if (idx == 0) {
      data->c2 = comp_page->meta.c2_start;
      last_c2 = data->c2;
    } else {
      if (comp_data->inc_c2) {
        ++last_c2;
      }
      data->c2 = last_c2;
    }

    // c3
    data->c3 = (((uint32_t)comp_data->c3_high) * (1 << 16)) + (uint32_t)comp_data->c3_low;

    // v1
    memcpy(data->v1, comp_data->v1, sizeof(comp_data->v1));
    data->v1[4] = 0;

    // v2: AA
    data->v2[0] = comp_data->v2 / 26 + 'A';
    data->v2[1] = comp_data->v2 % 26 + 'A';
    data->v2[2] = 0;

    // v3 - v5
    decode_9vischar(comp_data->v345, data->v3, data->v4, data->v5);

    // v6
    memcpy(data->v6, comp_data->v6, sizeof(comp_data->v6));
    data->v6[3] = 0;

    // v7 - v9
    decode_9vischar(comp_data->v789, data->v7, data->v8, data->v9);
  }
  return RC::SUCCESS;
}