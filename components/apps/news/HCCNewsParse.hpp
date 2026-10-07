/*
 * HCCNewsParse: tách tin từ RSS và nội dung bài báo từ HTML (không phụ thuộc ESP-IDF, kiểm thử được trên máy tính).
 *
 * RSS 2.0 / Atom: tiêu đề, link, tóm tắt, ảnh (thẻ <img> trong description, enclosure, media:content / thumbnail).
 * Bài báo: tìm khung nội dung theo dấu hiệu của các báo (vnexpress fck_detail, tuoitre / thanhnien detail-content,
 * dantri singular-content ...) rồi lấy đoạn văn, tiêu đề phụ, ảnh (ưu tiên data-src của ảnh tải chậm) và chú thích.
 * Không tìm được khung: lấy các đoạn <p> đủ dài trong trang và ảnh nằm giữa chúng.
 */
#pragma once

#include <stddef.h>
#include <string>
#include <vector>

namespace HCCNewsParse {

struct Item {
    std::string title;
    std::string link;
    std::string summary;
    std::string image;
    std::string date;
};

struct Block {
    enum Type { Para, Heading, Image, Caption };
    Type type;
    std::string text;  // Para / Heading / Caption
    std::string url;   // Image
};

std::vector<Item> parseRss(const char *xml, size_t len, size_t max_items = 60);
std::vector<Block> parseArticle(const char *html, size_t len, const std::string &page_url, size_t max_blocks = 400);

// Giải mã thực thể HTML (&amp; &#39; &#x1EA1; ...) và đổi ký tự trang trí (“ ” – … nbsp) sang ký tự font có.
std::string decodeEntities(const std::string &s);
// Bỏ thẻ HTML, giải mã thực thể, gộp khoảng trắng.
std::string htmlToText(const std::string &s);
std::string resolveUrl(const std::string &base, const std::string &href);

}  // namespace HCCNewsParse
