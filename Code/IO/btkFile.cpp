/* 
 * The Biomechanical ToolKit
 * Copyright (c) 2009-2015, Arnaud Barré
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 *     * Redistributions of source code must retain the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *     * Neither the name(s) of the copyright holders nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written
 *       permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "btkFile.h"
#include "btkFile_p.h"
#include "btkConfigure.h"
#include "btkLogger.h"
  
#if defined(HAVE_SYS_MMAP)
  #if defined(HAVE_64_BIT)
    #ifndef _LARGEFILE_SOURCE
      #define _LARGEFILE_SOURCE
    #endif
    #define _FILE_OFFSET_BITS 64 // Replace all the functions (mmap, lssek, ...) by their 64 bit version
  #endif
  #include <sys/mman.h> // mmap, munmap
  #include <sys/stat.h> // fstat
  #include <fcntl.h> // open, Close
  #include <unistd.h> // ftruncate
#endif

// -------------------------------------------------------------------------- //
//                                 PRIVATE API                                //
// -------------------------------------------------------------------------- //

namespace btk
{
  FilePrivate::FilePrivate()
  : IODevicePrivate(), Buffer(new MemoryMappedBuffer)
  {};
  
  FilePrivate::~FilePrivate() noexcept
  {
    if (this->Buffer->isOpen() && !this->Buffer->close())
      Logger::error("An error occurred when closing the file device. The data inside the file might be corrupted.");
    delete this->Buffer;
  }
  
  // ----------------------------------------------------------------------- //
  
  /**
   * Destructor
   */
  MemoryMappedBuffer::MemoryMappedBuffer() noexcept
  {
    this->mp_Data = 0;
    this->m_DataSize = -1;
    this->m_LogicalSize = 0;
    this->m_File = _BTK_MMFILEBUF_NO_FILE;
#if defined(_MSC_VER)
    this->m_Map = 0;
#endif
    this->m_Offset = -1;
    this->m_Writing = false;
  };
  
  /**
   * Destructor. Try to Close the file if opened.
   */
  
  /**
   * Open the file with the given filename @a s and the options @a mode.
   */
  MemoryMappedBuffer* MemoryMappedBuffer::open(const char* s, File::OpenMode mode) noexcept
  {
    assert(mode & ~std::ios_base::binary); // Impossible as binary is not proposed!
    if (this->isOpen())
      return 0;
    File::OpenMode m = mode & (~File::AtEnd);
    this->m_Writing = (m & File::Out) != 0;
    // Open the file and map it into the memory
    // The flags' extraction is inspired by the file fstream.cxx from the Comeau Computing library
#if defined(HAVE_SYS_MMAP) // POSIX
    // Select the flags for the function open
    int flags = 0;
    if ((m == File::Out) || (m == File::Out + File::Truncate))
      flags = O_RDWR | O_CREAT | O_TRUNC;
    else if (m == File::Out + File::Append)
      flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (m == File::In)
      flags = O_RDONLY;
    else if (m == File::In + File::Out)
      flags = O_RDWR;
    else if (m == File::In + File::Out + File::Truncate)
      flags = O_RDWR | O_CREAT | O_TRUNC;
    else // Other flags are not supported in the C++ standard
      return 0;
    // Open the file
    if ((this->m_File = ::open(s, flags, S_IRWXU)) == -1)
      return 0;
    // Get the size of the file
    struct stat info;
    if (::fstat(this->m_File, &info) == -1)
      return this->close();
    this->m_DataSize = this->m_LogicalSize = info.st_size;
    // New file or truncated file?
    if ((this->m_DataSize == 0) && this->m_Writing)
    {
      this->m_DataSize = MemoryMappedBuffer::granularity();
      if ((::lseek(this->m_File, this->m_DataSize-1, SEEK_SET) == -1)
           || (::write(this->m_File, "", 1) == -1))
        return this->close();
    }
#else // Windows
    // Select the flags for the function CreateFile
    DWORD dwDesiredAccess, dwShareMode, dwCreationDisposition, dwFlagsandAttributes;
    dwDesiredAccess = GENERIC_READ | GENERIC_WRITE;
    dwShareMode = FILE_SHARE_READ;
    dwFlagsandAttributes = FILE_ATTRIBUTE_TEMPORARY;
    switch(m)
    {
    case File::Out:
      dwCreationDisposition = OPEN_ALWAYS;
      break;
    case File::Out + File::Truncate:
      dwCreationDisposition = CREATE_ALWAYS;
      break;
    case File::Out + File::Append:
      dwCreationDisposition = OPEN_ALWAYS;
      break;
    case File::In:
      dwDesiredAccess = GENERIC_READ;
      dwShareMode = FILE_SHARE_READ | FILE_SHARE_WRITE; // Is it a good idea to give the possibility to other processes to write in the file that we are reading?
      dwCreationDisposition = OPEN_EXISTING;
      dwFlagsandAttributes = FILE_ATTRIBUTE_READONLY;
      break;
    case File::In + File::Out:
      dwCreationDisposition = OPEN_ALWAYS;
      break;
    case File::In + File::Out + File::Truncate:
      dwCreationDisposition = TRUNCATE_EXISTING;
      break;
    default: // Other flags are not supported in the C++ standard
      return 0;
    }
    // Open the file
    if ((this->m_File = ::CreateFile(s, dwDesiredAccess, dwShareMode, NULL, 
                                     dwCreationDisposition, dwFlagsandAttributes, NULL))
        == INVALID_HANDLE_VALUE)
      return 0;
    // Get the size of the file
    DWORD dwFileSizeHi = 0;
    DWORD dwFileSizeLo = GetFileSize(this->m_File, &dwFileSizeHi);
    this->m_DataSize = this->m_LogicalSize = static_cast<size_t>(((uint64_t)dwFileSizeHi << 32) | dwFileSizeLo);
    // New file or truncated file?
    if ((this->m_DataSize == 0) && this->m_Writing)
    {
      this->m_DataSize = MemoryMappedBuffer::granularity();
      LONG lDistHigh = (uint64_t)this->m_DataSize >> 32; // 32 = (sizeof(LONG) * 8)
      LONG lDistLow = this->m_DataSize & 0xFFFFFFFF;
      if (((::SetFilePointer(this->m_File, lDistLow, &lDistHigh, FILE_BEGIN) == INVALID_SET_FILE_POINTER) 
           && (::GetLastError() != NO_ERROR)) || (::SetEndOfFile(this->m_File) == 0))
        return this->close();
    }
#endif
    // Map the file
    if (!this->map())
      return this->close();
      
    this->m_Offset = 0;
    
    // If necessary go to the end of the file (AtEnd option)
    if ((mode & File::AtEnd) && (this->seek(0,File::End) == File::Position(File::Offset(-1))))
    {
      this->close();
      return 0;
    }
      
    return this;
  };
  
  /**
   * @fn bool MemoryMappedBuffer::isOpen() const noexcept
   * Return true if the file is opened.
   */
   
  /**
   * Close the file.
   * @return Return 0 is an error happened during the closing.
   */ 
  MemoryMappedBuffer* MemoryMappedBuffer::close() noexcept
  {
    if (!this->isOpen())
      return 0;

#if defined(HAVE_SYS_MMAP)
    bool err = !(::munmap(this->mp_Data, this->m_DataSize) == 0);
#else
    BOOL err = (::UnmapViewOfFile(this->mp_Data) == 0) || (::closeHandle(this->m_Map) == 0);
    this->m_Map = NULL;
#endif
    
    // If in write mode, truncate the file to the number of bytes wrote.
    if (this->m_Writing)
    {
#if defined(HAVE_SYS_MMAP)
      err |= (::ftruncate(this->m_File, this->m_LogicalSize) == -1);
#else
      LONG lDistHigh = (uint64_t)this->m_LogicalSize >> 32; // 32 = (sizeof(LONG) * 8)
      LONG lDistLow = this->m_LogicalSize & 0xFFFFFFFF;   
      err |= (((::SetFilePointer(this->m_File, lDistLow, &lDistHigh, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
               && (::GetLastError() != NO_ERROR)) || (::SetEndOfFile(this->m_File) == 0));
#endif
    }

    // Close the file
#if defined(HAVE_SYS_MMAP)
    err |= ::close(this->m_File);
#else
    err |= (::closeHandle(this->m_File) == 0);
#endif
    this->m_File = _BTK_MMFILEBUF_NO_FILE;
    
    if (err)
      return 0;
    this->mp_Data = 0;
    this->m_DataSize = 0;
    this->m_LogicalSize = 0;
    
    return this;
  };

  /**
   * @fn bool MemoryMappedBuffer::hasWriteMode() const  noexcept
   * Check if this file buffer is in write mode or not.
   */

  /**
   * @fn size_t MemoryMappedBuffer::dataSize() const  noexcept
   * Returns the size of the buffer.
   */
  
  /**
   * @fn const char* MemoryMappedBuffer::data() const  noexcept
   * Returns directly the content of the buffer.
   */
  
  /**
   * Sets internal position pointer to relative position.
   * @return The new position value of the modified position pointer. Errors are expected to be signaled by an invalid position value, like -1.
   */
  File::Position MemoryMappedBuffer::seek(File::Offset off, File::SeekDir way) noexcept
  {
    switch(way)
    {
    case File::Begin:
      if (off < 0)
        return -1;
      this->m_Offset = off;
      break;
    case File::Current:
      if (this->m_Offset + off < 0)
        return -1;
      this->m_Offset += off;
      break;
    case File::End:
      if (this->m_DataSize + off < 0)
        return -1;
      this->m_Offset = this->m_DataSize + off;
      break;
    default:
      return -1;
    }
    return this->m_Offset;
  };
  
 /**
  * Returns a sequence of characters
  * @return The number of characters gotten.
  */
  File::Size MemoryMappedBuffer::peek(char* s, File::Size n) const noexcept
  {
    n = (((this->m_Offset + n) == 0) || ((this->m_Offset + n) > this->m_DataSize)) ? ((this->m_DataSize - this->m_Offset) > 0 ? this->m_DataSize - this->m_Offset : 0) : n;
    for (Offset i = 0 ; i < n ; ++i)
      s[i] = this->mp_Data[this->m_Offset + i];
    return n;
  };
  
  /**
   * Get sequence of characters
   * @return The number of characters gotten, returned as a value of type streamsize.
   */
  File::Size MemoryMappedBuffer::read(char* s, File::Size n) noexcept
  {
    n = this->peek(s,n);
    this->m_Offset += n;
    return n;
  };
  
  /**
   * Write a sequence of characters
   * @return The number of characters written, returned as a value of type streamsize.
   */
  File::Size MemoryMappedBuffer::write(const char* s, File::Size n) noexcept
  {
    while ((this->m_Offset + n) > this->m_DataSize) 
    {
      if (!this->resizeMap())
        return 0;
    }
    
    for (File::Offset i = 0 ; i < n ; ++i)
      this->mp_Data[this->m_Offset + i] = s[i];
    this->m_Offset += n;
    
    if (this->m_Offset >= this->m_LogicalSize)
      this->m_LogicalSize = this->m_Offset;
    return n;
  };
  
  /**
   * Try to map the file into the memory.
   * @return Returns 0 if an error occured.
   */
  MemoryMappedBuffer* MemoryMappedBuffer::map() noexcept
  {
#if defined(HAVE_SYS_MMAP)
    bool err = ((this->mp_Data = (char*)::mmap(0, 
                                                 this->m_DataSize, 
                                                 this->m_Writing ? (PROT_READ | PROT_WRITE) : PROT_READ, 
                                                 MAP_SHARED, this->m_File, 0)) == MAP_FAILED);
#else
    BOOL err = ((this->m_Map = ::CreateFileMapping(this->m_File,
                                                   NULL, 
                                                   this->m_Writing ? PAGE_READWRITE : PAGE_READONLY,
                                                   0, 
                                                   0,
                                                   NULL)) == NULL);
    err |= ((this->mp_Data = (char*)::MapViewOfFile(this->m_Map,
                                                       this->m_Writing ? FILE_MAP_WRITE : FILE_MAP_READ,
                                                       0,
                                                       0,
                                                       this->m_DataSize)) == NULL);
#endif
    return err ? 0 : this;
  };
  
  /**
   * Try to resize the map to be able to extract more data from the file.
   * @return Returns 0 if an error occured.
   */
  MemoryMappedBuffer* MemoryMappedBuffer::resizeMap() noexcept
  {
    if (!this->isOpen() || !this->m_Writing)
      return 0;
    size_t newBufferSize = this->m_DataSize + this->granularity();
#if defined(_MSC_VER)
    if ((::UnmapViewOfFile(this->mp_Data) == 0) || (::closeHandle(this->m_Map) == 0))
      return 0;
    this->m_Map = NULL;
    LONG lDistHigh = (uint64_t)newBufferSize >> 32; // 32 = (sizeof(LONG) * 8)
    LONG lDistLow = newBufferSize & 0xFFFFFFFF;   
    if (((::SetFilePointer(this->m_File, lDistLow, &lDistHigh, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
               && (::GetLastError() != NO_ERROR)) || (::SetEndOfFile(this->m_File) == 0))
      return 0;
#else
    if (::munmap(this->mp_Data, this->m_DataSize) == -1)
          return 0;
    if (::ftruncate(this->m_File, newBufferSize) == -1)
      return 0;      
#endif
    this->m_DataSize = newBufferSize;
    return this->map();
  };
  
  /**
   * Return the size of block of memory.
   */
  int MemoryMappedBuffer::granularity() noexcept
  {
#if defined(_MSC_VER)
    SYSTEM_INFO info;
    ::GetSystemInfo(&info);
    return static_cast<int>(info.dwAllocationGranularity);
#else
    return static_cast<int>(::sysconf(_SC_PAGESIZE));
#endif
  };
}

// -------------------------------------------------------------------------- //
//                                 PUBLIC API                                 //
// -------------------------------------------------------------------------- //

namespace btk
{
  /**
   * @class File btkFile.h
   * @brief Device to read/write data from/to a file.
   *
   * Internally this class uses automatically a buffer mapped into computer's memory (see https://en.wikipedia.org/wiki/Memory-mapped_file).
   *
   * @warning Currently the OpenMode Append has no effect on this device.
   *
   * @ingroup BTKIO
   */
  
  /**
   * Default constructor. You must use the method Open after the use of this constructor
   */
  File::File()
  : IODevice(*new FilePrivate)
  {};
  
  /**
   * Destructor (default).
   */
  File::~File() noexcept = default;
  
  /**
   * Open the given @a fileName with the specified @a mode.
   * @note To open a file, the device has to be first closed if a previous file was already opened.
   */
  void File::open(const std::string& fileName, OpenMode mode)
  {
    if (this->verifyOpenMode(mode))
    {
      auto optr = this->pimpl();
      if (!optr->Buffer->open(fileName.c_str(), mode))
        this->setState(FailBit);
      else
        this->clear();
    }
  };
  
  /**
   * Returns true if a file was successfuly opened, otherwise false.
   */
  bool File::isOpen() const noexcept
  {
    auto optr = this->pimpl();
    return optr->Buffer->isOpen();
  };
  
  /**
   * The use of this method without any file associated will set the FailBit to true.
   */
  void File::close()
  {
    auto optr = this->pimpl();
    if (!optr->Buffer->close())
      this->setState(FailBit);
  };
  
  /**
   * Similar to the read() method but does not modify the position of the read indicator.
   * Returns the number of characters read. No exception or error flag is triggered when an error occurs.
   */
  File::Size File::peek(char* s, Size n) const
  {
    auto optr = this->pimpl();
    return optr->Buffer->peek(s,n);
  };
  
  /**
   * Gets from the device a sequence of characters of size @a n and store it in @a s. 
   * The FailBit state flag is set if any issue happens during the reading operation. 
   * In case the number or characters read does not correspond to the number of characters to read @a n, the EndBit state flag is also set.
   */
  void File::read(char* s, Size n)
  {
    auto optr = this->pimpl();
    if (optr->Buffer->read(s,n) != n)
      this->setState(FailBit | EndBit);
  };
  
  /**
   * Puts the sequence of characters @a s of size @a n to the device.
   * The ErrorBit state flag is set if any issue happens during the wrting operation.
   * In case you attempt to write on the device while it is open in read-only mode, the FailBit state flag will be set. 
   */
  void File::write(const char* s, Size n)
  {
    auto optr = this->pimpl();
    if (!optr->Buffer->hasWriteMode()) // Read-only?
      this->setState(FailBit);
    else if (optr->Buffer->write(s,n) != n)
      this->setState(ErrorBit);
  };
  
  /**
   * Moves the internal pointer of the given @a offset in the given direction @a dir.
   */
  void File::seek(Offset offset, SeekDir dir)
  {
    auto optr = this->pimpl();
    if (!this->hasFailure() && (optr->Buffer->seek(offset, dir) == Position(Offset(-1))))
      this->setState(FailBit);
  };
  
  /**
   * Returns the position of the internal pointer.
   */
  File::Position File::tell() const noexcept
  {
    auto optr = this->pimpl();
    return !this->hasFailure() ? optr->Buffer->seek(0, Current) : Position(Offset(-1));
  };
  
  /**
   * Returns false as the File class represents a random access device.
   */
  bool File::isSequential() const noexcept
  {
    return false;
  };
  
  /**
   * Return the data mapped in the memory
   */
  const char* File::data() const noexcept
  {
    auto optr = this->pimpl();
    return optr->Buffer->data();
  };
  
 /**
  * Return the size of the data mapped in the memory
  */
 File::Size File::size() const noexcept
 {
   auto optr = this->pimpl();
   return optr->Buffer->dataSize();
 };
  
};