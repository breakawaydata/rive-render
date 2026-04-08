#include "config.hpp"

#include <stdexcept>
#include <string>
#include <map>

// Minimal JSON parser — just enough for our config format.
// We avoid pulling in nlohmann/json to keep dependencies minimal.
// This parser handles the flat/nested object structure we need.

namespace
{

// Skip whitespace
size_t skipWs(const std::string& s, size_t i)
{
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                            s[i] == '\r'))
        ++i;
    return i;
}

// Parse a JSON string (assumes cursor is on opening quote)
std::string parseString(const std::string& s, size_t& i)
{
    if (s[i] != '"')
        throw std::runtime_error("Expected '\"'");
    ++i;
    std::string result;
    while (i < s.size() && s[i] != '"')
    {
        if (s[i] == '\\')
        {
            ++i;
            if (i >= s.size())
                break;
            switch (s[i])
            {
                case '"':
                    result += '"';
                    break;
                case '\\':
                    result += '\\';
                    break;
                case '/':
                    result += '/';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 't':
                    result += '\t';
                    break;
                default:
                    result += s[i];
                    break;
            }
        }
        else
        {
            result += s[i];
        }
        ++i;
    }
    if (i < s.size())
        ++i; // skip closing quote
    return result;
}

// Parse a JSON number
float parseNumber(const std::string& s, size_t& i)
{
    size_t start = i;
    if (s[i] == '-')
        ++i;
    while (i < s.size() && (std::isdigit(s[i]) || s[i] == '.' || s[i] == 'e' ||
                            s[i] == 'E' || s[i] == '+' || s[i] == '-'))
        ++i;
    return std::stof(s.substr(start, i - start));
}

// Parse a JSON boolean
bool parseBool(const std::string& s, size_t& i)
{
    if (s.substr(i, 4) == "true")
    {
        i += 4;
        return true;
    }
    if (s.substr(i, 5) == "false")
    {
        i += 5;
        return false;
    }
    throw std::runtime_error("Expected 'true' or 'false'");
}

// Skip a JSON value (string, number, bool, null, object, array)
void skipValue(const std::string& s, size_t& i)
{
    i = skipWs(s, i);
    if (s[i] == '"')
    {
        parseString(s, i);
    }
    else if (s[i] == '{')
    {
        int depth = 1;
        ++i;
        while (i < s.size() && depth > 0)
        {
            if (s[i] == '{')
                ++depth;
            else if (s[i] == '}')
                --depth;
            else if (s[i] == '"')
                { size_t tmp = i; parseString(s, tmp); i = tmp - 1; } // parseString increments past quote
            ++i;
        }
    }
    else if (s[i] == '[')
    {
        int depth = 1;
        ++i;
        while (i < s.size() && depth > 0)
        {
            if (s[i] == '[')
                ++depth;
            else if (s[i] == ']')
                --depth;
            else if (s[i] == '"')
                { size_t tmp = i; parseString(s, tmp); i = tmp - 1; }
            ++i;
        }
    }
    else if (s[i] == 't' || s[i] == 'f')
    {
        parseBool(s, i);
    }
    else if (s[i] == 'n')
    {
        i += 4; // null
    }
    else
    {
        parseNumber(s, i);
    }
}

// Parse a flat string->string map
std::map<std::string, std::string> parseStringMap(const std::string& s,
                                                   size_t& i)
{
    std::map<std::string, std::string> result;
    i = skipWs(s, i);
    if (s[i] != '{')
        throw std::runtime_error("Expected '{'");
    ++i;
    while (true)
    {
        i = skipWs(s, i);
        if (s[i] == '}')
        {
            ++i;
            break;
        }
        if (s[i] == ',')
            ++i;
        i = skipWs(s, i);
        auto key = parseString(s, i);
        i = skipWs(s, i);
        if (s[i] != ':')
            throw std::runtime_error("Expected ':'");
        ++i;
        i = skipWs(s, i);
        auto value = parseString(s, i);
        result[key] = value;
    }
    return result;
}

} // namespace

Config Config::parse(const std::string& json)
{
    Config cfg;
    size_t i = 0;
    i = skipWs(json, i);
    if (json[i] != '{')
        throw std::runtime_error("Expected JSON object");
    ++i;

    while (true)
    {
        i = skipWs(json, i);
        if (i >= json.size() || json[i] == '}')
            break;
        if (json[i] == ',')
            ++i;
        i = skipWs(json, i);
        if (json[i] == '}')
            break;

        auto key = parseString(json, i);
        i = skipWs(json, i);
        if (json[i] != ':')
            throw std::runtime_error("Expected ':'");
        ++i;
        i = skipWs(json, i);

        if (key == "rivFile")
        {
            cfg.rivFile = parseString(json, i);
        }
        else if (key == "artboard")
        {
            cfg.artboard = parseString(json, i);
        }
        else if (key == "stateMachine")
        {
            cfg.stateMachine = parseString(json, i);
        }
        else if (key == "width")
        {
            cfg.width = static_cast<int>(parseNumber(json, i));
        }
        else if (key == "height")
        {
            cfg.height = static_cast<int>(parseNumber(json, i));
        }
        else if (key == "ffmpegPath")
        {
            cfg.ffmpegPath = parseString(json, i);
        }
        else if (key == "useCommandQueue")
        {
            cfg.useCommandQueue = parseBool(json, i);
        }
        else if (key == "screenshot")
        {
            // Parse screenshot sub-object
            i = skipWs(json, i);
            if (json[i] != '{')
                throw std::runtime_error("Expected screenshot object");
            ++i;
            while (true)
            {
                i = skipWs(json, i);
                if (json[i] == '}')
                {
                    ++i;
                    break;
                }
                if (json[i] == ',')
                    ++i;
                i = skipWs(json, i);
                if (json[i] == '}')
                {
                    ++i;
                    break;
                }
                auto skey = parseString(json, i);
                i = skipWs(json, i);
                if (json[i] != ':')
                    throw std::runtime_error("Expected ':'");
                ++i;
                i = skipWs(json, i);
                if (skey == "path")
                    cfg.screenshot.path = parseString(json, i);
                else if (skey == "timestamp")
                    cfg.screenshot.timestamp = parseNumber(json, i);
                else
                    skipValue(json, i);
            }
        }
        else if (key == "output")
        {
            // Parse output sub-object
            i = skipWs(json, i);
            if (json[i] != '{')
                throw std::runtime_error("Expected output object");
            ++i;
            while (true)
            {
                i = skipWs(json, i);
                if (json[i] == '}')
                {
                    ++i;
                    break;
                }
                if (json[i] == ',')
                    ++i;
                i = skipWs(json, i);
                if (json[i] == '}')
                {
                    ++i;
                    break;
                }
                auto okey = parseString(json, i);
                i = skipWs(json, i);
                if (json[i] != ':')
                    throw std::runtime_error("Expected ':'");
                ++i;
                i = skipWs(json, i);
                if (okey == "format")
                    cfg.output.format = parseString(json, i);
                else if (okey == "path")
                    cfg.output.path = parseString(json, i);
                else if (okey == "fps")
                    cfg.output.fps = parseNumber(json, i);
                else if (okey == "duration")
                    cfg.output.duration = parseNumber(json, i);
                else if (okey == "quality")
                    cfg.output.quality = static_cast<int>(parseNumber(json, i));
                else
                    skipValue(json, i);
            }
        }
        else if (key == "assets")
        {
            // Parse assets sub-object
            i = skipWs(json, i);
            if (json[i] != '{')
                throw std::runtime_error("Expected assets object");
            ++i;
            while (true)
            {
                i = skipWs(json, i);
                if (json[i] == '}')
                {
                    ++i;
                    break;
                }
                if (json[i] == ',')
                    ++i;
                i = skipWs(json, i);
                if (json[i] == '}')
                {
                    ++i;
                    break;
                }
                auto akey = parseString(json, i);
                i = skipWs(json, i);
                if (json[i] != ':')
                    throw std::runtime_error("Expected ':'");
                ++i;
                i = skipWs(json, i);
                if (akey == "images")
                    cfg.assets.images = parseStringMap(json, i);
                else if (akey == "fonts")
                    cfg.assets.fonts = parseStringMap(json, i);
                else
                    skipValue(json, i);
            }
        }
        else
        {
            // Skip unknown keys
            skipValue(json, i);
        }
    }

    if (cfg.rivFile.empty())
    {
        throw std::runtime_error("rivFile is required");
    }

    return cfg;
}
