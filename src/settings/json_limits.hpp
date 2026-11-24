// vgmplay - WEWNETRZNY naglowek: parsowanie JSON z twardymi limitami glebokosci i liczby wartosci.
// Limity sa egzekwowane PODCZAS parsowania (SAX), zanim zbudowane zostanie cale DOM: plik
// z 100000 zagniezdzonych '[' albo z miliardem elementow konczy sie bledem po kilku krokach.
// (Wariant z parser_callback_t mial zlozonosc kwadratowa przy 200000 wpisow: 62 s - patrz ADR 0003.)
#pragma once

#include <new>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "vgmplay/core.hpp"

namespace vgmplay::fsutil {

namespace detail {

// Nakladka na standardowy budowniczy DOM: liczy glebokosc i wartosci, przy przekroczeniu zwraca false
// (sax_parse przerywa prace). Pozostale zdarzenia deleguje do json_sax_dom_parser.
class LimitedDomSax final : public nlohmann::json_sax<nlohmann::json> {
public:
    using json = nlohmann::json;
    LimitedDomSax(json& root, int maxDepth, std::size_t maxValues)
        : dom_(root, /*allow_exceptions=*/false), maxDepth_(maxDepth), maxValues_(maxValues)
    {
    }

    enum class Hit { None, TooDeep, TooMany };
    [[nodiscard]] Hit hit() const noexcept { return hit_; }

    bool null() override { return scalar() && dom_.null(); }
    bool boolean(bool v) override { return scalar() && dom_.boolean(v); }
    bool number_integer(json::number_integer_t v) override { return scalar() && dom_.number_integer(v); }
    bool number_unsigned(json::number_unsigned_t v) override { return scalar() && dom_.number_unsigned(v); }
    bool number_float(json::number_float_t v, const json::string_t& s) override { return scalar() && dom_.number_float(v, s); }
    bool string(json::string_t& v) override { return scalar() && dom_.string(v); }
    bool binary(json::binary_t& v) override { return scalar() && dom_.binary(v); }
    bool start_object(std::size_t n) override { return open() && dom_.start_object(n); }
    bool key(json::string_t& v) override { return dom_.key(v); }
    bool end_object() override
    {
        --depth_;
        return dom_.end_object();
    }
    bool start_array(std::size_t n) override { return open() && dom_.start_array(n); }
    bool end_array() override
    {
        --depth_;
        return dom_.end_array();
    }
    bool parse_error(std::size_t pos, const std::string& tok, const nlohmann::detail::exception& e) override
    {
        return dom_.parse_error(pos, tok, e);
    }

private:
    bool scalar()
    {
        if (++count_ > maxValues_) { hit_ = Hit::TooMany; return false; }
        return true;
    }
    bool open()
    {
        if (++depth_ > maxDepth_) { hit_ = Hit::TooDeep; return false; }
        return scalar();
    }

    nlohmann::detail::json_sax_dom_parser<json, nlohmann::detail::iterator_input_adapter<const char*>> dom_;
    int maxDepth_;
    std::size_t maxValues_;
    int depth_ = 0;
    std::size_t count_ = 0;
    Hit hit_ = Hit::None;
};

} // namespace detail

// Parsuje `text`. Bledy: ParseError (skladnia), LimitExceeded (zagniezdzenie > maxDepth poziomow,
// root = poziom 1, lub liczba wartosci > maxValues). Nie rzuca.
[[nodiscard]] inline Result<nlohmann::json> parseJsonLimited(std::string_view text, int maxDepth, std::size_t maxValues)
{
    using nlohmann::json;
    try {
        json j;
        detail::LimitedDomSax sax(j, maxDepth, maxValues);
        const bool ok = json::sax_parse(text.data(), text.data() + text.size(), &sax, json::input_format_t::json, /*strict=*/true);
        if (sax.hit() == detail::LimitedDomSax::Hit::TooDeep) return makeError(ErrorCode::LimitExceeded, "JSON nesting too deep");
        if (sax.hit() == detail::LimitedDomSax::Hit::TooMany) return makeError(ErrorCode::LimitExceeded, "JSON has too many values");
        if (!ok) return makeError(ErrorCode::ParseError, "invalid JSON");
        return j;
    } catch (const std::bad_alloc&) {
        return makeError(ErrorCode::OutOfMemory, "out of memory parsing JSON");
    } catch (const std::exception& ex) {
        return makeError(ErrorCode::ParseError, std::string("JSON: ") + ex.what());
    }
}

} // namespace vgmplay::fsutil
