#ifndef __EPUB_BOOK_H__
#define __EPUB_BOOK_H__

#include "Book.h"
#include <string>
#include <map>
#include <vector>

// Forward declaration for libxml2 node type (header-only usage)
struct _xmlNode;
typedef struct _xmlNode xmlNode;

typedef struct epub_image_t
{
    std::string path;          // path inside EPUB
    int display_w;              // display width
    int display_h;              // display height
} epub_image_t;

typedef struct epub_t
{
    std::string path;
    std::string ocf;
    std::string opf;
    std::string ncx;
    manifests_t manifests;
    spines_t spines;
    navpoints_t navpoints;
} epub_t;


class EpubBook : public Book
{
public:
    EpubBook();
    virtual ~EpubBook();

public:
    virtual book_type_t GetBookType(void);
    virtual BOOL SaveBook(HWND hWnd);
    virtual BOOL UpdateChapters(int offset);
    int GetImageCount(void) { return (int)m_Images.size(); }
    epub_image_t* GetImage(int index) { return (index >= 0 && index < (int)m_Images.size()) ? &m_Images[index] : NULL; }
    Gdiplus::Bitmap* DecodeImage(int index);
    virtual BOOL GetInlineImage(int img_idx, Gdiplus::Bitmap **bmp, int *w, int *h);

protected:
    virtual BOOL ParserBook(HWND hWnd);
    virtual Gdiplus::Bitmap* GetCover(void);
    virtual int GetTextBeginIndex(void);
    void FreeFilelist(void);
    BOOL UnzipBook(void);
    BOOL ParserOcf(epub_t &epub);
    BOOL ParserOpf(epub_t &epub);
    BOOL ParserNcx(epub_t &epub);
    BOOL ParserOps(file_data_t *fdata, wchar_t **text, int *len, wchar_t **title, int *tlen, BOOL parsertitle);
    BOOL ParserChapters(epub_t &epub);
    BOOL ParserCover(epub_t &epub);
    BOOL WalkBodyNodes(xmlNode *node, wchar_t **text, int *len);
    static void AppendText(wchar_t **text, int *len, const wchar_t *src, int srclen);

protected:
    Gdiplus::Bitmap *m_Cover;
    filelist_t m_flist;
    std::string m_EpubPath;
    std::string m_CurChapterPath;   // dir of current chapter, relative to epub root (e.g. "Text/")
    std::vector<epub_image_t> m_Images;
    std::vector<Gdiplus::Bitmap*> m_ImageCache;
};

#endif
