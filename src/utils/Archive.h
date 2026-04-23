/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct archive;
struct archive_entry;

// Progress callback payload used by OpenArchiveFromFile in eagerLoad mode.
// nDecoded : count of files (entries) processed so far (incremented whether
//            decompression succeeded or failed).
// nTotal   : total count of files when known, -1 while the archive header
//            is still being iterated. For libarchive this only becomes
//            known at the end, so most callbacks carry -1 during the pass
//            and a final callback with nDecoded == nTotal.
struct ArchiveExtractProgress {
    int nDecoded;
    int nTotal;
};

using ArchiveExtractProgressCb = Func1<ArchiveExtractProgress*>;

class MultiFormatArchive {
  public:
    enum class Format {
        Unknown,
        Zip,
        Rar,
        SevenZip,
        Tar
    };

    struct FileInfo {
        size_t fileId = 0;
        const char* name = nullptr;
        i64 fileTime = 0; // this is typedef'ed as time64_t in unrar.h
        size_t fileSizeUncompressed = 0;
        bool isDir = false;
        // set when eagerLoad extraction failed for this entry (bad data,
        // OOM, etc.). `data` will be nullptr in that case.
        bool failed = false;

        // internal use
        i64 filePos = 0;
        char* data = nullptr;

        FILETIME GetWinFileTime() const;
    };

    MultiFormatArchive();
    ~MultiFormatArchive();

    Format format = Format::Unknown;

    // hintKind is the result of a prior GuessFileTypeFromContent() done
    // by the caller. When non-null we skip the internal 2 KiB sniff and
    // use it to drive rar-first vs. libarchive routing.
    // cbProgress != nullptr signals "eager load": decompress every entry
    // at open time and close the archive so no re-open will ever happen.
    // The callback fires after each entry is processed (see
    // ArchiveExtractProgress); pass &emptyCb to turn on eager load without
    // actually wanting notifications.
    bool Open(const char* path, Kind hintKind = nullptr, const ArchiveExtractProgressCb* cbProgress = nullptr);
    bool Open(IStream* stream);

    Vec<FileInfo*> const& GetFileInfos();

    size_t GetFileId(const char* fileName);

    // Return the FileInfo record for a given entry, loading its data into
    // fileInfo->data on demand (on a miss, re-opens the archive unless
    // that was disabled by eager-load mode).
    //
    // Ownership: the returned FileInfo* is owned by this archive. By
    // default fileInfo->data is *not* transferred to the caller — a later
    // call for the same entry returns the same cached buffer, and the
    // archive destructor frees it. If the caller wants the buffer to
    // outlive the archive, they should set fileInfo->data = nullptr after
    // saving the pointer; they then become responsible for free()ing it.
    //
    // Returns nullptr for an unknown name / out-of-range fileId. For an
    // entry whose decompression failed check fileInfo->failed — data will
    // be nullptr in that case.
    FileInfo* GetFileDataByName(const char* filename);
    FileInfo* GetFileDataById(size_t fileId);
    ByteSlice GetFileDataPartById(size_t fileId, size_t sizeHint);

    const char* GetComment();

    // password for encrypted archives (owned by this object)
    char* password = nullptr;

    // set after Open() if the archive contains encrypted entries
    bool isEncrypted = false;

  protected:
    // used for allocating strings that are referenced by ArchFileInfo::name
    Arena* allocator_ = nullptr;
    Vec<FileInfo*> fileInfos_;

    char* archivePath_ = nullptr;

    // only set when we loaded file infos using unrar.dll fallback
    const char* rarFilePath_ = nullptr;

    bool OpenArchive(const char* path, const ArchiveExtractProgressCb* cbProgress);
    bool ParseEntries(struct archive* a, const ArchiveExtractProgressCb* cbProgress);

    bool OpenUnrarFallback(const char* rarPathUtf, const ArchiveExtractProgressCb* cbProgress);
    // Populate fileInfos_[fileId]->data via the respective backend; set
    // ->failed when extraction didn't produce the expected bytes.
    void LoadFileDataByIdUnarrDll(size_t fileId);
    void LoadFileDataByIdLibarchive(size_t fileId);
    ByteSlice GetFileDataPartByIdUnarrDll(size_t fileId, size_t sizeHint);
    bool LoadedUsingUnrarDll() const { return rarFilePath_ != nullptr; }
};

// Open a file on disk. MultiFormatArchive::Open(path) detects RAR via a
// content sniff and routes it through unrar.dll; everything else goes
// through libarchive.
//
// When cbProgress is non-null every file is decompressed during Open()
// and the archive is then closed — GetFileDataById for a file that failed
// to decompress returns an empty ByteSlice and never re-opens the file.
// Use FileInfo::failed to tell "not yet loaded" from "failed". Pass &emptyCb
// (a default-constructed ArchiveExtractProgressCb) to enable eager load
// without actually wanting progress notifications.
MultiFormatArchive* OpenArchiveFromFile(const char* path, const ArchiveExtractProgressCb* cbProgress = nullptr);

// Open from an IStream. libarchive auto-detects the container (zip/rar/
// 7z/tar/etc.) — no per-format wrapper needed.
MultiFormatArchive* OpenArchiveFromStream(IStream* stream);
