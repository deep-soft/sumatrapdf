/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct archive;
struct archive_entry;

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
    bool Open(const char* path, Kind hintKind = nullptr);
    bool Open(IStream* stream);

    Vec<FileInfo*> const& GetFileInfos();

    size_t GetFileId(const char* fileName);

    ByteSlice GetFileDataByName(const char* filename);
    ByteSlice GetFileDataById(size_t fileId);
    ByteSlice GetFileDataPartById(size_t fileId, size_t sizeHint);

    const char* GetComment();

    // if true, will load and uncompress all files on open
    bool loadOnOpen = false;

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

    bool OpenArchive(const char* path);
    bool ParseEntries(struct archive* a);

    bool OpenUnrarFallback(const char* rarPathUtf);
    ByteSlice GetFileDataByIdUnarrDll(size_t fileId);
    ByteSlice GetFileDataPartByIdUnarrDll(size_t fileId, size_t sizeHint);
    ByteSlice GetFileDataByIdLibarchive(size_t fileId);
    bool LoadedUsingUnrarDll() const { return rarFilePath_ != nullptr; }
};

// Open a file on disk. MultiFormatArchive::Open(path) detects RAR via a
// content sniff and routes it through unrar.dll; everything else goes
// through libarchive.
MultiFormatArchive* OpenArchiveFromFile(const char* path);

// Open from an IStream. libarchive auto-detects the container (zip/rar/
// 7z/tar/etc.) — no per-format wrapper needed.
MultiFormatArchive* OpenArchiveFromStream(IStream* stream);
