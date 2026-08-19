#include "EpubBook.h"
#include "Utils.h"
#include <stdio.h>
#include <windows.h>
#ifdef ZLIB_ENABLE
#include "unzip.h"
#include "iowin32.h"
#else
#include "miniz.h"
#endif
#include "libxml/xmlreader.h"
#include "libxml/HTMLtree.h"
#include "libxml/HTMLparser.h"
#include "libxml/xpath.h"
#include <shlwapi.h>


EpubBook::EpubBook()
    : m_Cover(NULL)
{
    xmlInitParser();
}

EpubBook::~EpubBook()
{
    ForceKill();
    FreeFilelist();
    if (m_Cover)
    {
        delete m_Cover;
    }
    // Free cached inline image bitmaps
    for (size_t i = 0; i < m_ImageCache.size(); i++)
    {
        if (m_ImageCache[i])
        {
            delete m_ImageCache[i];
            m_ImageCache[i] = NULL;
        }
    }
    m_ImageCache.clear();
    xmlCleanupParser();
}

book_type_t EpubBook::GetBookType()
{
    return book_epub;
}

BOOL EpubBook::SaveBook(HWND hWnd)
{
    return FALSE;
}

BOOL EpubBook::UpdateChapters(int offset)
{
    return FALSE;
}

BOOL EpubBook::ParserBook(HWND hWnd)
{
    BOOL ret = FALSE;
    epub_t epub;
    manifests_t::iterator itor;
    navpoints_t::iterator it;

    // unzip epub file
    if (!UnzipBook())
        goto end;

    // parser epub file
    epub.ocf = "META-INF/container.xml";
    if (!ParserOcf(epub))
        goto end;

    if (!ParserOpf(epub))
        goto end;

    if (!ParserNcx(epub))
        goto end;

    // parser epub cover image
    ParserCover(epub);

    // Parser epub chapters & text
    if (!ParserChapters(epub))
        goto end;

    ret = TRUE;

end:
    // free epub
    for (itor = epub.manifests.begin(); itor != epub.manifests.end(); itor++)
    {
        delete itor->second;
    }
    epub.manifests.clear();
    for (it = epub.navpoints.begin(); it != epub.navpoints.end(); it++)
    {
        delete it->second;
    }
    epub.navpoints.clear();
    epub.spines.clear();
    // Pre-decode all inline images NOW while m_flist is still populated.
    // FreeFilelist() below releases all file data; DecodeImage() is called
    // again at render time and must hit m_ImageCache, not m_flist.
    for (size_t ii = 0; ii < m_Images.size(); ii++)
    {
        DecodeImage((int)ii);
    }
    FreeFilelist();
    if (!ret)
    {
        if (m_Cover)
        {
            delete m_Cover;
            m_Cover = NULL;
        }
        CloseBook();
    }

    return ret;
}

Gdiplus::Bitmap* EpubBook::GetCover(void)
{
    return m_Cover;
}

int EpubBook::GetTextBeginIndex(void)
{
    return m_Cover ? 1 : 0;
}

void EpubBook::FreeFilelist(void)
{
    filelist_t::iterator itor;
    for (itor = m_flist.begin(); itor != m_flist.end(); itor++)
    {
        free(itor->second.data);
    }
    m_flist.clear();
}

#ifdef ZLIB_ENABLE
BOOL EpubBook::UnzipBook(void)
{
    unzFile uf = NULL;
    zlib_filefunc64_def ffunc = {0};
    uLong i;
    unz_global_info gi = {0};
    int err = UNZ_ERRNO;
    unz_file_info64 file_info = {0};
    char filename_inzip[MAX_PATH] = {0};
    char *buf = NULL;
    file_data_t fdata;

    fill_win32_filefunc64W(&ffunc);
    uf = unzOpen2_64(m_fileName, &ffunc);
    if (!uf)
        goto end;

    err = unzGetGlobalInfo(uf, &gi);
    if (err != UNZ_OK)
        goto end;

    FreeFilelist();

    for (i = 0; i < gi.number_entry; i++)
    {
        // current file
        {
            err = unzGetCurrentFileInfo64(uf, &file_info, filename_inzip, sizeof(filename_inzip), NULL, 0, NULL, 0);
            if (err != UNZ_OK)
                goto end;

            if (filename_inzip[file_info.size_filename - 1] == '\\' || filename_inzip[file_info.size_filename - 1] == '/')
            {
                // is directory
            }
            else
            {
                // check password
                err = unzOpenCurrentFilePassword(uf, NULL);
                if (err != UNZ_OK)
                    goto end;

                // create memory to save this file
                buf = (char *)malloc((size_t)file_info.uncompressed_size);
                if (!buf)
                    goto end;

                // read file to memory
                err = unzReadCurrentFile(uf, buf, (unsigned int)file_info.uncompressed_size);
                if (err != file_info.uncompressed_size)
                    goto end;
                err = UNZ_OK;

                // save to file map
                fdata.data = buf;
                fdata.size = (int)file_info.uncompressed_size;
                m_flist.insert(std::make_pair(filename_inzip, fdata));
                buf = NULL;
            }
            unzCloseCurrentFile(uf);
        }

        // next file
        if ((i + 1) < gi.number_entry)
        {
            err = unzGoToNextFile(uf);
            if (err != UNZ_OK)
                goto end;
        }

        if (m_bForceKill)
        {
            err = UNZ_ERRNO;
            goto end;
        }
    }

end:
    if (uf)
    {
        unzCloseCurrentFile(uf);
        unzClose(uf);
    }
    if (buf)
        free(buf);

    if (err != UNZ_OK)
        FreeFilelist();
    return err == UNZ_OK;
}
#else
BOOL EpubBook::UnzipBook(void)
{
    mz_zip_archive zip_archive;
    mz_zip_archive_file_stat file_stat;
    mz_uint file_count = 0;
    mz_uint i;
#if 0
    char* filename = NULL;
#endif
    char *buf = NULL;
    file_data_t fdata;

#if 0
    filename = Utf16ToAnsi(m_fileName);
#endif

    FreeFilelist();

    memset(&zip_archive, 0, sizeof(zip_archive));

    if (!mz_zip_reader_init_file(&zip_archive, (const char*)m_fileName, 0))
        return FALSE;

    file_count = mz_zip_reader_get_num_files(&zip_archive);
    if (file_count == 0)
    {
        mz_zip_reader_end(&zip_archive);
        return FALSE;
    }

    if (!mz_zip_reader_file_stat(&zip_archive, 0, &file_stat))
    {
        mz_zip_reader_end(&zip_archive);
        return FALSE;
    }

    for (i = 0; i < file_count; i++)
    {
        if (!mz_zip_reader_file_stat(&zip_archive, i, &file_stat))
            continue;
        if (mz_zip_reader_is_file_a_directory(&zip_archive, i))
            continue; // skip directories for now

        // create memory to save this file
        buf = (char *)malloc((size_t)file_stat.m_uncomp_size);
        if (!buf)
        {
            mz_zip_reader_end(&zip_archive);
            FreeFilelist();
            return FALSE;
        }

        // read file to memory
        if (!mz_zip_reader_extract_to_mem(&zip_archive, i, buf, (size_t)file_stat.m_uncomp_size, 0))
        {
            free(buf);
            mz_zip_reader_end(&zip_archive);
            FreeFilelist();
            return FALSE;
        }

        // save to file map
        fdata.data = buf;
        fdata.size = (size_t)file_stat.m_uncomp_size;
        m_flist.insert(std::make_pair(file_stat.m_filename, fdata));
    }

    // Close the archive, freeing any resources it was using
    mz_zip_reader_end(&zip_archive);
    return TRUE;
}
#endif

BOOL EpubBook::ParserOcf(epub_t &epub)
{
    filelist_t::iterator itor;
    xmlDocPtr doc = NULL;
    const xmlChar *xpath = NULL;
    xmlXPathContextPtr xpathctx = NULL;
    xmlXPathObjectPtr xpathobj = NULL;
    xmlNodeSetPtr nodeset;
    xmlChar *keyword;
    int i;
    BOOL ret = FALSE;

    itor = m_flist.find(epub.ocf);
    if (itor == m_flist.end())
        goto end;

    doc = xmlReadMemory((const char *)itor->second.data, itor->second.size, NULL, NULL, XML_PARSE_RECOVER | XML_PARSE_NOBLANKS);
    if (!doc)
        goto end;

    if (m_bForceKill)
        goto end;

    xpathctx = xmlXPathNewContext(doc);
    if (!xpathctx)
        goto end;

    if (m_bForceKill)
        goto end;

    xpath = BAD_CAST("//*[local-name()='rootfile']/@full-path");
    xpathobj = xmlXPathEvalExpression(xpath, xpathctx);
    if (!xpathobj)
        goto end;

    if (xmlXPathNodeSetIsEmpty(xpathobj->nodesetval))
        goto end;

    nodeset = xpathobj->nodesetval;
    for (i = 0; i < nodeset->nodeNr; i++)
    {
        keyword = xmlNodeGetContent(nodeset->nodeTab[i]);

        if (keyword && keyword[0])
        {
            epub.opf = (const char *)keyword;
            if (epub.opf.find('/'))
            {
                epub.path = epub.opf.substr(0, epub.opf.rfind('/') + 1);
                m_EpubPath = epub.path;
            }
            ret = TRUE;
            xmlFree(keyword);
            break;
        }
        if (keyword)
            xmlFree(keyword);
    }

end:
    if (xpathobj)
        xmlXPathFreeObject(xpathobj);
    if (xpathctx)
        xmlXPathFreeContext(xpathctx);
    if (doc)
        xmlFreeDoc(doc);

    return ret;
}

BOOL EpubBook::ParserOpf(epub_t &epub)
{
    filelist_t::iterator itor;
    xmlDocPtr doc = NULL;
    const xmlChar *xpath = NULL;
    xmlXPathContextPtr xpathctx = NULL;
    xmlXPathObjectPtr xpathobj = NULL;
    xmlNodeSetPtr nodeset;
    xmlChar *keyword;
    manifest_t *item = NULL;
    manifests_t::iterator it;
    int i,j;
    BOOL ret = FALSE;
    char buff[1024];

    itor = m_flist.find(epub.opf);
    if (itor == m_flist.end())
        goto end;

    doc = xmlReadMemory((const char *)itor->second.data, itor->second.size, NULL, NULL, XML_PARSE_RECOVER | XML_PARSE_NOBLANKS);
    if (!doc)
        goto end;

    if (m_bForceKill)
        goto end;

    xpathctx = xmlXPathNewContext(doc);
    if (!xpathctx)
        goto end;

    if (m_bForceKill)
        goto end;

    // parser manifest
    {
        xpath = BAD_CAST("//*[local-name()='item']/@*[name()='id' or name()='href' or name()='media-type']");
        xpathobj = xmlXPathEvalExpression(xpath, xpathctx);
        if (!xpathobj)
            goto end;

        if (xmlXPathNodeSetIsEmpty(xpathobj->nodesetval))
            goto end;

        nodeset = xpathobj->nodesetval;
        for (i = 0; i < nodeset->nodeNr; /*i++*/)
        {
            item = new manifest_t;
            if (!item)
                goto end;
            for (j = 0; j < 3 && i < nodeset->nodeNr; j++, i++)
            {
                keyword = xmlNodeGetContent(nodeset->nodeTab[i]);
                if (keyword && keyword[0])
                {
                    if (xmlStrcmp(nodeset->nodeTab[i]->name, (const xmlChar *)"id") == 0)
                    {
                        item->id = (const char *)keyword;
                    }
                    else if (xmlStrcmp(nodeset->nodeTab[i]->name, (const xmlChar *)"href") == 0)
                    {
                        url_decode((const char *)keyword, buff);
                        item->href = (const char *)buff;
                    }
                    else if (xmlStrcmp(nodeset->nodeTab[i]->name, (const xmlChar *)"media-type") == 0)
                    {
                        item->media_type = (const char *)keyword;
                    }
                }
                if (keyword)
                    xmlFree(keyword);
                if (m_bForceKill)
                {
                    delete item;
                    goto end;
                }
            }
            if (!item->id.empty() && !item->href.empty() && !item->media_type.empty())
            {
                epub.manifests.insert(std::make_pair(item->id, item));
            }
            else
            {
                delete item;
                goto end;
            }
        }

        if (epub.manifests.empty())
            goto end;

        if (xpathobj)
            xmlXPathFreeObject(xpathobj);
    }
    
    if (m_bForceKill)
        goto end;

    // parser spine
    {
        xpath = BAD_CAST("//*[local-name()='itemref']/@idref");
        xpathobj = xmlXPathEvalExpression(xpath, xpathctx);
        if (!xpathobj)
            goto end;

        if (xmlXPathNodeSetIsEmpty(xpathobj->nodesetval))
            goto end;

        nodeset = xpathobj->nodesetval;
        for (i = 0; i < nodeset->nodeNr; i++)
        {
            keyword = xmlNodeGetContent(nodeset->nodeTab[i]);
            if (keyword && keyword[0])
            {
                if (epub.manifests.find((const char *)keyword) == epub.manifests.end())
                {
                    xmlFree(keyword);
                    goto end;
                }
                epub.spines.push_back((const char *)keyword);
            }
            if (keyword)
                xmlFree(keyword);
            if (m_bForceKill)
                goto end;
        }

        if (epub.spines.empty())
            goto end;

        if (xpathobj)
            xmlXPathFreeObject(xpathobj);
    }

    ret = TRUE;

    if (m_bForceKill)
    {
        ret = FALSE;
        goto end;
    }

    // parser ncx, ncx is not a required file
    {
        xpath = BAD_CAST("//*[local-name()='spine']/@toc");
        xpathobj = xmlXPathEvalExpression(xpath, xpathctx);
        if (!xpathobj)
            goto end;

        if (xmlXPathNodeSetIsEmpty(xpathobj->nodesetval))
            goto end;

        nodeset = xpathobj->nodesetval;
        for (i = 0; i < nodeset->nodeNr; i++)
        {
            keyword = xmlNodeGetContent(nodeset->nodeTab[i]);
            if (keyword && keyword[0])
            {
                it = epub.manifests.find((const char *)keyword);
                if (it == epub.manifests.end())
                {
                    xmlFree(keyword);
                    epub.ncx = "";
                    break;
                }
                epub.ncx = it->second->href;
                break;
            }
            if (keyword)
                xmlFree(keyword);
        }
    }

end:
    if (xpathobj)
        xmlXPathFreeObject(xpathobj);
    if (xpathctx)
        xmlXPathFreeContext(xpathctx);
    if (doc)
        xmlFreeDoc(doc);

    return ret;
}

BOOL EpubBook::ParserNcx(epub_t &epub)
{
    filelist_t::iterator itor;
    xmlDocPtr doc = NULL;
    xmlNodePtr node;
    xmlChar *order, *id, *text, *src;
    const xmlChar *xpath = NULL;
    xmlXPathContextPtr xpathctx = NULL;
    xmlXPathObjectPtr xpathobj = NULL;
    xmlNodeSetPtr nodeset;
    navpoint_t *navp = NULL;
    int i;
    BOOL ret = FALSE;
    int flag = 0;
    char buff[1024];

    // not exist ncx
    if (epub.ncx.empty())
        return TRUE;

    itor = m_flist.find(epub.path+epub.ncx);
    if (itor == m_flist.end())
        goto end;

    doc = xmlReadMemory((const char *)itor->second.data, itor->second.size, NULL, NULL, XML_PARSE_RECOVER | XML_PARSE_NOBLANKS);
    if (!doc)
        goto end;

    if (m_bForceKill)
        goto end;

    xpathctx = xmlXPathNewContext(doc);
    if (!xpathctx)
        goto end;

    if (m_bForceKill)
        goto end;

    xpath = BAD_CAST("//*[local-name()='navPoint']");
    xpathobj = xmlXPathEvalExpression(xpath, xpathctx);
    if (!xpathobj)
        goto end;

    if (xmlXPathNodeSetIsEmpty(xpathobj->nodesetval))
        goto end;

    nodeset = xpathobj->nodesetval;
    for (i = 0; i < nodeset->nodeNr; i++)
    {
        src = NULL;
        text = NULL;
        node = nodeset->nodeTab[i];
        id = xmlGetProp(node, BAD_CAST"id");
        order = xmlGetProp(node, BAD_CAST"playOrder");
        //text = xmlNodeGetContent(node);
        node = node->children;
        flag = 0;
        while (node)
        {
            if (xmlStrcasecmp(node->name, BAD_CAST"content") == 0)
            {
                src = xmlGetProp(node, BAD_CAST"src");
                flag++;
            }
            if (xmlStrcasecmp(node->name, BAD_CAST"navLabel") == 0)
            {
                text = xmlNodeGetContent(node->children);
                flag++;
            }
            if (flag >= 2)
                break;
            node = node->next;
        }

#if 0 // do not check this
        if (!id || !order || !text || !src
            || !id[0] || !order[0] || !text[0] || !src[0])
        {
            if (id)
                xmlFree(id);
            if (order)
                xmlFree(order);
            if (text)
                xmlFree(text);
            if (src)
                xmlFree(src);
            goto end;
        }
#endif

        navp = new navpoint_t;
        if (id)
            navp->id = (const char *)id;
        if (order)
            navp->order = atoi((const char *)order);
        if (text)
            navp->text = (const char *)text;
        if (src)
        {
            url_decode((const char *)src, buff);
            navp->src = (const char *)buff;
        }
        epub.navpoints.insert(std::make_pair(navp->src, navp));

        if (id)
            xmlFree(id);
        if (order)
            xmlFree(order);
        if (text)
            xmlFree(text);
        if (src)
            xmlFree(src);
        if (m_bForceKill)
            goto end;
    }

    if (!epub.navpoints.empty())
    {
        ret = TRUE;
    }

end:
    if (xpathobj)
        xmlXPathFreeObject(xpathobj);
    if (xpathctx)
        xmlXPathFreeContext(xpathctx);
    if (doc)
        xmlFreeDoc(doc);
    return ret;
}

// Append wide text to buffer, reallocating as needed
void EpubBook::AppendText(wchar_t **text, int *len, const wchar_t *src, int srclen)
{
    if (!src || srclen <= 0) return;
    wchar_t *p = (wchar_t*)realloc(*text, (*len + srclen + 1) * sizeof(wchar_t));
    if (!p) return;
    memcpy(p + *len, src, srclen * sizeof(wchar_t));
    *text = p;
    *len += srclen;
    (*text)[*len] = L'\0';
}

// Recursively walk HTML DOM nodes to extract text and detect <img> elements.
// For each <img> inserts IMAGE_MARKER (0xFFFC) into the text and records the image path.
BOOL EpubBook::WalkBodyNodes(xmlNode *node, wchar_t **text, int *len)
{
    BOOL ok = TRUE;
    for (xmlNode *cur = node; cur; cur = cur->next)
    {
        if (m_bForceKill)
            return FALSE;
        if (cur->type == XML_TEXT_NODE && cur->content)
        {
            // Use xmlNodeGetContent() instead of raw cur->content:
            // htmlDocDumpMemoryFormat serializes U+2014/curly quotes as
            // numeric entities (&#8212; / &#8220;), and xmlNodeGetContent
            // decodes those entities back to the real characters. Raw
            // cur->content can keep the half-decoded/entity form, which
            // makes em-dashes and quotes vanish on screen (TXT unaffected).
            wchar_t *decoded = NULL;
            int decoded_len = 0;
            xmlChar *node_content = xmlNodeGetContent(cur);
            if (!node_content)
                continue;
            const char *src = (const char*)node_content;
            int srcsize = (int)strlen(src);
            if (!DecodeText(src, srcsize, &decoded, &decoded_len))
            {
                ok = FALSE;
            }
            else if (decoded && decoded_len > 0)
            {
                AppendText(text, len, decoded, decoded_len);
                free(decoded);
            }
            xmlFree(node_content);
        }
        else if (cur->type == XML_ELEMENT_NODE)
        {
            const char *tag = (const char*)cur->name;
            // Recover-mode parse can produce nodes with NULL name - skip safely
            if (tag == NULL)
            {
                if (cur->children)
                    WalkBodyNodes(cur->children, text, len);
                continue;
            }
            if (strcmp(tag, "img") == 0)
            {
                xmlChar *src = xmlGetProp(cur, (const xmlChar*)"src");
                if (src)
                {
                    // Insert marker character
                    wchar_t marker = (wchar_t)IMAGE_MARKER;
                    AppendText(text, len, &marker, 1);
                    // Record image path: resolve relative to current chapter dir,
                    // normalize "." and ".." segments against epub root
                    std::string rel = (const char*)src;
                    if (!m_CurChapterPath.empty())
                        rel = m_CurChapterPath + rel;
                    // Resolve path segments with a stack
                    std::vector<std::string> segs;
                    size_t pos = 0;
                    while (pos <= rel.size())
                    {
                        size_t slash = rel.find('/', pos);
                        std::string seg = (slash == std::string::npos)
                            ? rel.substr(pos)
                            : rel.substr(pos, slash - pos);
                        if (seg == "..")
                        {
                            if (!segs.empty() && segs.back() != "..")
                                segs.pop_back();
                            // else: leading ".." beyond root - drop it
                        }
                        else if (!seg.empty() && seg != ".")
                        {
                            segs.push_back(seg);
                        }
                        if (slash == std::string::npos)
                            break;
                        pos = slash + 1;
                    }
                    // Rebuild: epub root (m_EpubPath) + remaining segments
                    std::string norm = m_EpubPath;
                    for (size_t s = 0; s < segs.size(); s++)
                    {
                        if (s > 0 || (!norm.empty() && norm[norm.size()-1] != '/'))
                            norm += '/';
                        norm += segs[s];
                    }
                    epub_image_t img;
                    img.path = norm;
                    img.display_w = 0;
                    img.display_h = 0;
                    m_Images.push_back(img);
                    xmlFree(src);
                }
            }
            else if (strcmp(tag, "br") == 0)
            {
                wchar_t nl = L'\n';
                AppendText(text, len, &nl, 1);
            }
            else
            {
                // Recurse into children
                if (cur->children)
                {
                    if (!WalkBodyNodes(cur->children, text, len))
                        ok = FALSE;
                }
                // Add newline after block-level elements
                const char *block_tags[] = {"p","div","h1","h2","h3","h4","h5","h6","blockquote","li","pre","hr",NULL};
                for (int b = 0; block_tags[b]; b++)
                {
                    if (strcmp(tag, block_tags[b]) == 0)
                    {
                        wchar_t nl = L'\n';
                        AppendText(text, len, &nl, 1);
                        break;
                    }
                }
            }
        }
    }
    return ok;
}

BOOL EpubBook::ParserOps(file_data_t *fdata, wchar_t **text, int *len, wchar_t **title, int *tlen, BOOL parsertitle)
{
    filelist_t::iterator itor;
    xmlDocPtr doc = NULL;
    xmlChar *value;
    const xmlChar *xpath = NULL;
    xmlXPathContextPtr xpathctx = NULL;
    xmlXPathObjectPtr xpathobj = NULL;
    xmlNodeSetPtr nodeset;
    int i;
    BOOL ret = FALSE;
    xmlChar *format_str = NULL;
    int size;

    xmlKeepBlanksDefault(0);
    // Explicit UTF-8: htmlReadMemory with NULL encoding ignores the
    // <?xml?> declaration and can decode multi-byte text as Latin-1,
    // which corrupts/drops U+2014 em-dashes (破折号) after the
    // dump->reparse round-trip below.
    doc = htmlReadMemory((const char *)fdata->data, fdata->size, NULL, "UTF-8", XML_PARSE_RECOVER | XML_PARSE_NOBLANKS);
    if (!doc)
        goto end;

    if (m_bForceKill)
        goto end;

#if 1 // format xml
    //xmlDocDumpFormatMemory(doc, &format_str, &size, 1);
    htmlDocDumpMemoryFormat(doc, &format_str, &size, 1);
    xmlFreeDoc(doc);
    if (!format_str || size <= 0)
    {
        if (format_str)
            xmlFree(format_str);
        goto end;
    }
    
    xmlKeepBlanksDefault(1);
    doc = xmlReadMemory((const char *)format_str, size, NULL, NULL, XML_PARSE_RECOVER/* | XML_PARSE_NOBLANKS*/);
    xmlFree(format_str);
    if (!doc)
        goto end;
#endif
    
    xpathctx = xmlXPathNewContext(doc);
    if (!xpathctx)
        goto end;

    if (m_bForceKill)
        goto end;

    if (parsertitle)
    {
        // parser title
        xpath = BAD_CAST("//*[local-name()='title']");
        xpathobj = xmlXPathEvalExpression(xpath, xpathctx);
        if (!xpathobj)
            goto body;

        if (xmlXPathNodeSetIsEmpty(xpathobj->nodesetval))
            goto body;

        nodeset = xpathobj->nodesetval;
        for (i = 0; i < nodeset->nodeNr; i++)
        {
            value = xmlNodeGetContent(nodeset->nodeTab[i]);

            if (!DecodeText((const char *)value, (int)strlen((const char *)value), title, tlen))
            {
                xmlFree(value);
                break;
            }
            xmlFree(value);
            break;
        }
    }

    if (m_bForceKill)
        goto end;

body:
    if (xpathobj)
        xmlXPathFreeObject(xpathobj);
    {
        // parser body + extract images
        xpath = BAD_CAST("//*[local-name()='body']");
        xpathobj = xmlXPathEvalExpression(xpath, xpathctx);
        if (!xpathobj)
            goto end;

        if (xmlXPathNodeSetIsEmpty(xpathobj->nodesetval))
            goto end;

        nodeset = xpathobj->nodesetval;
        for (i = 0; i < nodeset->nodeNr; i++)
        {
            // Walk body children recursively to extract text + images.
            // Failure here is NOT fatal - some nodes may fail to decode
            // but the chapter text is still usable.
            WalkBodyNodes(nodeset->nodeTab[i], text, len);
            break;
        }
        ret = TRUE;
    }

end:
    if (xpathobj)
        xmlXPathFreeObject(xpathobj);
    if (xpathctx)
        xmlXPathFreeContext(xpathctx);
    if (doc)
        xmlFreeDoc(doc);
    return ret;
}

BOOL EpubBook::ParserChapters(epub_t &epub)
{
    typedef struct buffer_t
    {
        wchar_t *text;
        int len;
    } buffer_t;

    spines_t::iterator itspine;
    filelist_t::iterator itflist;
    manifests_t::iterator itmfest;
    navpoints_t::iterator itnav;
    file_data_t *fdata;
    chapter_item_t chapter;
    std::string filename;
    wchar_t *text = NULL;
    wchar_t *title = NULL;
    int len = 0, tlen = 0;
    int index = 0, i=0;
    buffer_t *buffer = NULL;

    m_Length = GetCover() ? 1 : 0;
    buffer = (buffer_t *)malloc(epub.spines.size() * sizeof(buffer_t));
    for (itspine = epub.spines.begin(); itspine != epub.spines.end(); itspine++)
    {
        if (m_bForceKill)
            goto end;
        itmfest = epub.manifests.find(*itspine);
        if (itmfest != epub.manifests.end())
        {
            filename = epub.path + itmfest->second->href;
            // Set current chapter dir (relative to epub root) for image src resolution
            std::string href = itmfest->second->href;
            size_t slash = href.rfind('/');
            if (slash != std::string::npos)
                m_CurChapterPath = href.substr(0, slash + 1);
            else
                m_CurChapterPath.clear();
            itflist = m_flist.find(filename);
            itnav = epub.navpoints.find(itmfest->second->href);
            if (itflist != m_flist.end() /*&& itnav != epub.navmap.end()*/)
            {
                fdata = &(itflist->second);
                // Reset per-chapter buffers: AppendText reallocs/appends,
                // so each chapter must start from NULL/0 (unlike original
                // DecodeText which mallocs fresh each call).
                text = NULL;
                len = 0;
                title = NULL;
                tlen = 0;
                int img_before = (int)m_Images.size();
                if (ParserOps(fdata, &text, &len, &title, &tlen, itnav == epub.navpoints.end()))
                {
                    if (len > 0)
                    {
                        buffer[index].text = text;
                        buffer[index].len = len;
                        chapter.index = m_Length;
                        m_Length += len;
                        if (itnav != epub.navpoints.end())
                        {
                            DecodeText(itnav->second->text.c_str(), (int)itnav->second->text.size(), &title, &tlen);
                            chapter.title = title;
                        }
                        else
                        {
                            chapter.title = title;
                        }
                        if (title)
                            free(title);
                        chapter.title_len = tlen;
                        if (!chapter.title.empty())
                            m_Chapters.push_back(chapter);
                        index++;
                    }
                    else
                    {
                        // Chapter produced no text; drop any images collected for it
                        m_Images.resize(img_before);
                        if (text) { free(text); text = NULL; }
                        len = 0;
                    }
                }
                else
                {
                    // ParserOps failed; roll back images collected during this chapter
                    m_Images.resize(img_before);
                    if (text) { free(text); text = NULL; }
                    len = 0;
                }
            }
        }
    }

end:
    if (m_bForceKill)
    {
        for (i = 0; i < index; i++)
        {
            free(buffer[i].text);
        }
        free(buffer);
        return FALSE;
    }
    if (index > 0)
    {
        if (GetCover())
        {
            len = 1; // add one wchar_t '0x0a' new line for cover
            m_Text = (wchar_t *)malloc(sizeof(wchar_t) * (m_Length + 1));
            m_Text[0] = 0x0A;
        }
        else
        {
            len = 0;
            m_Text = (wchar_t *)malloc(sizeof(wchar_t) * (m_Length + 1));
        }
        m_Text[m_Length] = 0;
        
        for (i = 0; i < index; i++)
        {
            memcpy(m_Text + len, buffer[i].text, buffer[i].len * sizeof(wchar_t));
            len += buffer[i].len;
            free(buffer[i].text);
        }
    }
    free(buffer);
    return TRUE;
}

BOOL EpubBook::ParserCover(epub_t &epub)
{
    filelist_t::iterator itflist;
    manifests_t::iterator itmfest;
    navpoints_t::iterator itnav;
    navpoint_t *p_navpoint;
    file_data_t *fdata;
    IStream *pStream = NULL;
    const char *cover_fname = NULL;
    char image_fname[1024] = {0};

    if (m_Cover)
    {
        delete m_Cover;
        m_Cover = NULL;
    }

    // find 'cover' from ncx
    if (!epub.navpoints.empty())
    {
        for (itnav = epub.navpoints.begin(); itnav != epub.navpoints.end(); itnav++)
        {
            p_navpoint = itnav->second;
            if (p_navpoint->order <= 1)
            {
                if (strcasestr(p_navpoint->id.c_str(), "cover")
                    || strcasestr(p_navpoint->src.c_str(), "cover"))
                {
                    for (itmfest = epub.manifests.begin(); itmfest != epub.manifests.end(); itmfest++)
                    {
                        if (0 == strcmp(p_navpoint->src.c_str(), itmfest->second->href.c_str()))
                        {
                            cover_fname = itmfest->second->href.c_str();
                            goto _found;
                        }
                    }
                    break;
                }
            }
        }
    }

    // find 'cover' from spine
    if (!epub.spines.empty())
    {
        if (strcasestr(epub.spines.begin()->c_str(), "cover"))
        {
            itmfest = epub.manifests.find(*epub.spines.begin());
            if (itmfest != epub.manifests.end())
            {
                cover_fname = itmfest->second->href.c_str();
                goto _found;
            }
        }
    }

    // find 'cover' from manifest list
    for (itmfest = epub.manifests.begin(); itmfest != epub.manifests.end(); itmfest++)
    {
        if (strcasestr(itmfest->first.c_str(), "cover") && strcasestr(itmfest->second->media_type.c_str(), "image/"))
        {
            strcpy(image_fname, itmfest->second->href.c_str());
            goto _complete;
        }
    }

_found:
    // parser cover img from cover.xhtml
    if (cover_fname)
    {
        itflist = m_flist.find(epub.path + cover_fname);
        if (itflist != m_flist.end())
        {
            xmlDocPtr doc = NULL;
            xmlNodePtr node;
            xmlXPathContextPtr xpathctx = NULL;
            xmlXPathObjectPtr xpathobj = NULL;
            const xmlChar *xpath = NULL;
            xmlNodeSetPtr nodeset;
            int i;
            xmlChar *src, *href;

            fdata = &(itflist->second);
            doc = xmlReadMemory((const char *)fdata->data, fdata->size, NULL, NULL, XML_PARSE_RECOVER | XML_PARSE_NOBLANKS);
            if (doc)
            {
                xpathctx = xmlXPathNewContext(doc);
                if (xpathctx)
                {
                    xpath = BAD_CAST("//*[local-name()='img']");
                    xpathobj = xmlXPathEvalExpression(xpath, xpathctx);
                    if (!xpathobj || xmlXPathNodeSetIsEmpty(xpathobj->nodesetval))
                    {
                        if (xpathobj)
                            xmlXPathFreeObject(xpathobj);
                        xpath = BAD_CAST("//*[local-name()='image']");
                        xpathobj = xmlXPathEvalExpression(xpath, xpathctx);
                    }

                    if (xpathobj)
                    {
                        if (!xmlXPathNodeSetIsEmpty(xpathobj->nodesetval))
                        {
                            nodeset = xpathobj->nodesetval;
                            for (i = 0; i < nodeset->nodeNr; i++)
                            {
                                node = nodeset->nodeTab[i];
                                src = xmlGetProp(node, BAD_CAST"src");
                                href = xmlGetProp(node, BAD_CAST"href");

                                if (src)
                                {
                                    url_decode((const char *)src, image_fname);
                                    // to absolute path, need to do ...
                                    while (strstr(image_fname, "../") == image_fname)
                                    {
                                        strncpy(image_fname, image_fname+3, strlen(image_fname)-2);
                                    }
                                }
                                else if (href)
                                {
                                    url_decode((const char *)href, image_fname);
                                    // to absolute path, need to do ...
                                    while (strstr(image_fname, "../") == image_fname)
                                    {
                                        strncpy(image_fname, image_fname+3, strlen(image_fname)-2);
                                    }
                                }
                                break;
                            }
                        }
                        xmlXPathFreeObject(xpathobj);
                    }
                    xmlXPathFreeContext(xpathctx);
                }
                xmlFreeDoc(doc);
            }
        }
    }

_complete:
    if (image_fname[0])
    {
        itflist = m_flist.find(epub.path + image_fname);
        if (itflist != m_flist.end())
        {
            fdata = &(itflist->second);
            pStream = SHCreateMemStream((const BYTE *)fdata->data, fdata->size);
            m_Cover = new Gdiplus::Bitmap(pStream);
            if (m_Cover)
            {
                if (Gdiplus::Ok != m_Cover->GetLastStatus())
                {
                    delete m_Cover;
                    m_Cover = NULL;
                }
            }
            pStream->Release();
        }
    }

    return m_Cover != NULL;
}

Gdiplus::Bitmap* EpubBook::DecodeImage(int index)
{
    if (index < 0 || index >= (int)m_Images.size())
        return NULL;
    epub_image_t &img = m_Images[index];
    if (img.path.empty())
        return NULL;

    // Return cached bitmap if already decoded
    if (index < (int)m_ImageCache.size() && m_ImageCache[index])
        return m_ImageCache[index];

    IStream *pStream = NULL;
    // img.path is already resolved against the epub root (see WalkBodyNodes)
    filelist_t::iterator it = m_flist.find(img.path);
    if (it == m_flist.end())
    {
        FILE *f = fopen("reader_img_debug.log", "a");
        if (f) { fprintf(f, "[Reader] image NOT FOUND: '%s' | flist size=%d | epub root='%s'\n",
                         img.path.c_str(), (int)m_flist.size(), m_EpubPath.c_str()); fclose(f); }
        return NULL;
    }
    file_data_t *fdata = &(it->second);
    pStream = SHCreateMemStream((const BYTE *)fdata->data, fdata->size);
    if (!pStream)
        return NULL;
    Gdiplus::Bitmap *bmp = new Gdiplus::Bitmap(pStream);
    if (!bmp || Gdiplus::Ok != bmp->GetLastStatus())
    {
        FILE *f = fopen("reader_img_debug.log", "a");
        if (f) { fprintf(f, "[Reader] DECODE FAIL idx=%d path='%s' status=%d size=%u\n",
                         index, img.path.c_str(), (int)(bmp ? bmp->GetLastStatus() : -1),
                         (unsigned)fdata->size); fclose(f); }
        delete bmp;
        pStream->Release();
        return NULL;
    }
    img.display_w = bmp->GetWidth();
    img.display_h = bmp->GetHeight();
    pStream->Release();

    {
        FILE *f = fopen("reader_img_debug.log", "a");
        if (f) { fprintf(f, "[Reader] DECODE OK idx=%d path='%s' %dx%d size=%u\n",
                         index, img.path.c_str(), img.display_w, img.display_h,
                         (unsigned)fdata->size); fclose(f); }
    }

    // Cache it for reuse (owned by m_ImageCache, freed in destructor)
    if (index >= (int)m_ImageCache.size())
        m_ImageCache.resize(m_Images.size(), NULL);
    m_ImageCache[index] = bmp;
    return bmp;
}

BOOL EpubBook::GetInlineImage(int img_idx, Gdiplus::Bitmap **bmp, int *w, int *h)
{
    if (img_idx < 0 || img_idx >= (int)m_Images.size())
        return FALSE;
    epub_image_t &img = m_Images[img_idx];
    Gdiplus::Bitmap *bitmap = DecodeImage(img_idx);
    if (!bitmap)
        return FALSE;
    *bmp = bitmap;
    *w = img.display_w;
    *h = img.display_h;
    return TRUE;
}