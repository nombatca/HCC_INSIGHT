#pragma once

#include <string>
#include "HCCPanicTypes.hpp"

// Phân tích RAW panic log (.ips JSON hoặc text thô) thành cấu trúc chuẩn.
class HCCPanicParser {
public:
    static ParsedPanic parse(const std::string &raw);

private:
    static ParsedPanic parseJson(const std::string &raw);
    static ParsedPanic parseText(const std::string &raw);
    static void extractFromPanicString(ParsedPanic &out, const std::string &panic);
    static void buildSignature(ParsedPanic &out);
};
