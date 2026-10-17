// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wootz_authoring/wootz_authoring_store.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/flat_set.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_ANDROID)
#include "base/android/jni_android.h"
#include "base/android/jni_string.h"
#include "chrome/android/chrome_jni_headers/WootzAuthoringPathUtils_jni.h"
#endif
#include "crypto/sha2.h"
#include "net/base/url_util.h"
#include "net/http/http_request_headers.h"
#include "services/network/public/cpp/data_element.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom-shared.h"
#include "url/third_party/mozilla/url_parse.h"

namespace wootz_authoring {
namespace {

std::string Sha256Hex(std::string_view input) {
  return base::HexEncode(crypto::SHA256HashString(input));
}

std::string PayloadDigest(std::string_view payload) {
  return "sha256_" + Sha256Hex(payload);
}

std::string TimeToJsonString(base::Time time) {
  return base::NumberToString(time.InMillisecondsSinceUnixEpoch());
}

constexpr double kMinScoredReplayMatch = 0.72;

bool IsLowValueFieldName(std::string_view key) {
  if (key.starts_with("_")) {
    return true;
  }

  static constexpr std::string_view kVolatileFieldNames[] = {
      "token",                  // auth token (per session)
      "client_req_id",          // per-request / per-search-session id
      "fp",                     // runtime/browser fingerprint bucket
      "search_session_id",      // per-search-session id
      "max_ts",                 // search time cutoff (drifts)
      "cached_latest_updates",  // client message-cache state (IndexedDB)
      "oldest",                 // history window (client cache state)
      "latest",                 // history window (client cache state)
  };
  for (std::string_view volatile_name : kVolatileFieldNames) {
    if (key == volatile_name) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> RequestBodyBytesForKey(
    const network::ResourceRequest& request) {
  if (!request.request_body || request.request_body->elements()->empty()) {
    return std::nullopt;
  }

  std::string body;
  for (const network::DataElement& element :
       *request.request_body->elements()) {
    if (element.type() != network::DataElement::Tag::kBytes) {
      return std::nullopt;
    }
    body.append(element.As<network::DataElementBytes>().AsStringView());
  }
  return body;
}

std::string ValueToComparableString(const base::Value& value) {
  if (value.is_string()) {
    return value.GetString();
  }
  if (value.is_bool()) {
    return value.GetBool() ? "true" : "false";
  }
  if (value.is_int()) {
    return base::NumberToString(value.GetInt());
  }
  if (value.is_double()) {
    return base::NumberToString(value.GetDouble());
  }

  std::string json;
  if (base::JSONWriter::Write(value, &json)) {
    return json;
  }
  return std::string();
}

base::DictValue ParseFormFields(std::string_view input) {
  base::DictValue fields;
  url::Component query(input);
  url::Component key;
  url::Component value;
  while (url::ExtractQueryKeyValue(input, &query, &key, &value)) {
    std::string field_key(input.substr(key.begin, key.len));
    if (field_key.empty()) {
      continue;
    }
    std::string field_value(input.substr(value.begin, value.len));
    fields.Set(
        base::UnescapeURLComponent(
            field_key, base::UnescapeRule::NORMAL |
                           base::UnescapeRule::REPLACE_PLUS_WITH_SPACE),
        base::UnescapeURLComponent(
            field_value, base::UnescapeRule::NORMAL |
                             base::UnescapeRule::REPLACE_PLUS_WITH_SPACE));
  }
  return fields;
}

base::DictValue ParseJsonObjectFields(std::string_view input) {
  base::DictValue fields;
  std::optional<base::Value> parsed =
      base::JSONReader::Read(input, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return fields;
  }

  for (const auto [key, value] : parsed->GetDict()) {
    if (!key.empty()) {
      fields.Set(key, ValueToComparableString(value));
    }
  }
  return fields;
}

std::string TrimAsciiWhitespace(std::string_view input) {
  return std::string(base::TrimWhitespaceASCII(input, base::TRIM_ALL));
}

std::optional<std::string> GetMultipartBoundary(std::string_view content_type) {
  std::vector<std::string_view> parts = base::SplitStringPiece(
      content_type, ";", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  for (size_t i = 1; i < parts.size(); ++i) {
    std::string_view part = base::TrimWhitespaceASCII(parts[i], base::TRIM_ALL);
    constexpr std::string_view kBoundaryPrefix = "boundary=";
    if (!base::StartsWith(part, kBoundaryPrefix,
                          base::CompareCase::INSENSITIVE_ASCII)) {
      continue;
    }
    part.remove_prefix(kBoundaryPrefix.size());
    if (part.size() >= 2 && part.front() == '"' && part.back() == '"') {
      part = part.substr(1, part.size() - 2);
    }
    if (!part.empty()) {
      return std::string(part);
    }
  }
  return std::nullopt;
}

std::optional<std::string> ParseMultipartFieldName(
    std::string_view content_disposition) {
  constexpr std::string_view kNameToken = "name=\"";
  size_t name_start = content_disposition.find(kNameToken);
  if (name_start == std::string_view::npos) {
    return std::nullopt;
  }
  name_start += kNameToken.size();
  size_t name_end = content_disposition.find('"', name_start);
  if (name_end == std::string_view::npos || name_end <= name_start) {
    return std::nullopt;
  }
  return std::string(
      content_disposition.substr(name_start, name_end - name_start));
}

base::DictValue ParseMultipartFormFields(std::string_view body,
                                           std::string_view content_type) {
  base::DictValue fields;
  std::optional<std::string> boundary = GetMultipartBoundary(content_type);
  if (!boundary.has_value()) {
    return fields;
  }

  const std::string delimiter = "--" + *boundary;
  size_t search_offset = 0;
  while (true) {
    size_t delimiter_pos = body.find(delimiter, search_offset);
    if (delimiter_pos == std::string_view::npos) {
      break;
    }

    size_t part_start = delimiter_pos + delimiter.size();
    if (part_start + 1 < body.size() && body.substr(part_start, 2) == "--") {
      break;
    }
    if (part_start + 2 <= body.size() && body.substr(part_start, 2) == "\r\n") {
      part_start += 2;
    } else if (part_start < body.size() && body[part_start] == '\n') {
      ++part_start;
    }

    size_t next_delimiter_pos = body.find(delimiter, part_start);
    if (next_delimiter_pos == std::string_view::npos) {
      break;
    }

    std::string_view part =
        body.substr(part_start, next_delimiter_pos - part_start);
    search_offset = next_delimiter_pos;

    size_t header_end = part.find("\r\n\r\n");
    size_t separator_size = 4;
    if (header_end == std::string_view::npos) {
      header_end = part.find("\n\n");
      separator_size = 2;
    }
    if (header_end == std::string_view::npos) {
      continue;
    }

    std::string_view headers = part.substr(0, header_end);
    std::string_view value = part.substr(header_end + separator_size);
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
      value.remove_suffix(1);
    }

    std::optional<std::string> field_name;
    for (std::string_view header_line : base::SplitStringPiece(
             headers, "\n", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      header_line = base::TrimWhitespaceASCII(header_line, base::TRIM_TRAILING);
      if (base::StartsWith(header_line, "Content-Disposition:",
                           base::CompareCase::INSENSITIVE_ASCII)) {
        field_name = ParseMultipartFieldName(header_line);
        break;
      }
    }

    if (field_name.has_value() && !field_name->empty()) {
      fields.Set(*field_name, TrimAsciiWhitespace(value));
    }
  }

  return fields;
}

base::DictValue ParseRequestBodyFields(std::string_view body,
                                         std::string_view content_type) {
  const size_t first_non_space = body.find_first_not_of(" \t\r\n");
  if (first_non_space == std::string_view::npos) {
    return base::DictValue();
  }
  if (base::StartsWith(content_type, "multipart/form-data",
                       base::CompareCase::INSENSITIVE_ASCII)) {
    base::DictValue multipart_fields =
        ParseMultipartFormFields(body, content_type);
    if (!multipart_fields.empty()) {
      return multipart_fields;
    }
  }
  if (base::StartsWith(content_type, "application/json",
                       base::CompareCase::INSENSITIVE_ASCII) ||
      body[first_non_space] == '{') {
    base::DictValue json_fields = ParseJsonObjectFields(body);
    if (!json_fields.empty()) {
      return json_fields;
    }
  }
  if (base::StartsWith(content_type, "application/x-www-form-urlencoded",
                       base::CompareCase::INSENSITIVE_ASCII) ||
      body.find('=') != std::string_view::npos) {
    base::DictValue form_fields = ParseFormFields(body);
    if (!form_fields.empty()) {
      return form_fields;
    }
  }
  return base::DictValue();
}

base::DictValue QueryFieldsForUrlInternal(const GURL& url) {
  base::DictValue fields;
  for (net::QueryIterator it(url); !it.IsAtEnd(); it.Advance()) {
    std::string key(it.GetKey());
    if (!key.empty()) {
      fields.Set(key, it.GetUnescapedValue());
    }
  }
  return fields;
}

std::string EndpointKeyForUrl(const GURL& url) {
  GURL endpoint_url = url.GetWithoutRef();
  GURL::Replacements replacements;
  replacements.ClearQuery();
  return endpoint_url.ReplaceComponents(replacements).spec();
}

// True when the URL path looks like a static asset (identified by its path, so
// its query is a volatile cache-buster). Intentionally excludes .json, whose
// query is often a real identity.
bool IsStaticAssetPath(const GURL& url) {
  static constexpr std::string_view kStaticExtensions[] = {
      ".js", ".css", ".woff", ".woff2", ".ttf", ".otf", ".png", ".jpg",
      ".jpeg", ".gif", ".webp", ".svg", ".ico", ".mp4", ".webm", ".m3u8"};
  const std::string_view path = url.path();
  for (std::string_view ext : kStaticExtensions) {
    if (base::EndsWith(path, ext, base::CompareCase::INSENSITIVE_ASCII)) {
      return true;
    }
  }
  return false;
}

std::vector<std::pair<std::string, std::string>> DictToSortedPairs(
    const base::DictValue& dict,
    bool drop_low_value_fields) {
  std::vector<std::pair<std::string, std::string>> fields;
  for (const auto [key, value] : dict) {
    if (drop_low_value_fields && IsLowValueFieldName(key)) {
      continue;
    }
    fields.emplace_back(key, ValueToComparableString(value));
  }
  std::sort(fields.begin(), fields.end());
  return fields;
}

std::string SerializeFieldsForKey(const base::DictValue& dict,
                                  bool drop_low_value_fields) {
  std::string serialized;
  for (const auto& [key, value] :
       DictToSortedPairs(dict, drop_low_value_fields)) {
    serialized += key;
    serialized += '=';
    serialized += value;
    serialized += '\n';
  }
  return serialized;
}

std::string MakeNormalizedRequestKeyInternal(
    std::string_view method,
    const GURL& url,
    const base::DictValue& request_body_fields,
    std::string_view request_body_digest) {
  const base::DictValue query_fields = QueryFieldsForUrlInternal(url);
  std::string normalized_input;
  normalized_input += method;
  normalized_input += '\n';
  normalized_input += EndpointKeyForUrl(url);
  normalized_input += '\n';
  normalized_input += SerializeFieldsForKey(query_fields,
                                            /*drop_low_value_fields=*/true);
  normalized_input += "--body--\n";
  if (!request_body_fields.empty()) {
    normalized_input += SerializeFieldsForKey(request_body_fields,
                                              /*drop_low_value_fields=*/true);
  } else {
    normalized_input += request_body_digest;
    normalized_input += '\n';
  }
  return "sha256:" + Sha256Hex(normalized_input);
}

std::string EndpointIndexKey(std::string_view method,
                             std::string_view endpoint_key) {
  return std::string(method) + "\n" + std::string(endpoint_key);
}

bool StringToFiniteDouble(std::string_view input, double* output) {
  std::string input_string(input);
  return base::StringToDouble(input_string, output) && std::isfinite(*output);
}

size_t LevenshteinDistance(std::string_view lhs, std::string_view rhs) {
  if (lhs.empty()) {
    return rhs.size();
  }
  if (rhs.empty()) {
    return lhs.size();
  }

  std::vector<size_t> previous(rhs.size() + 1);
  std::vector<size_t> current(rhs.size() + 1);
  for (size_t i = 0; i <= rhs.size(); ++i) {
    previous[i] = i;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    current[0] = i + 1;
    for (size_t j = 0; j < rhs.size(); ++j) {
      const size_t replace_cost = previous[j] + (lhs[i] == rhs[j] ? 0 : 1);
      const size_t insert_cost = current[j] + 1;
      const size_t delete_cost = previous[j + 1] + 1;
      current[j + 1] = std::min({replace_cost, insert_cost, delete_cost});
    }
    previous.swap(current);
  }
  return previous[rhs.size()];
}

double StringSimilarity(std::string_view lhs, std::string_view rhs) {
  if (lhs == rhs) {
    return 1.0;
  }
  if (lhs.empty() || rhs.empty()) {
    return 0.0;
  }

  double lhs_number = 0.0;
  double rhs_number = 0.0;
  if (StringToFiniteDouble(lhs, &lhs_number) &&
      StringToFiniteDouble(rhs, &rhs_number)) {
    const double diff = std::abs(lhs_number - rhs_number);
    if (diff == 0.0) {
      return 1.0;
    }
    return std::max(0.0, 0.75 / (1.0 + std::log10(diff + 1.0) * 0.25));
  }

  constexpr size_t kMaxLevenshteinInput = 128;
  if (lhs.size() > kMaxLevenshteinInput || rhs.size() > kMaxLevenshteinInput) {
    size_t common_prefix = 0;
    const size_t max_prefix = std::min(lhs.size(), rhs.size());
    while (common_prefix < max_prefix &&
           lhs[common_prefix] == rhs[common_prefix]) {
      ++common_prefix;
    }
    return 0.7 * static_cast<double>(common_prefix) /
           static_cast<double>(std::max(lhs.size(), rhs.size()));
  }

  const size_t distance = LevenshteinDistance(lhs, rhs);
  const size_t max_size = std::max(lhs.size(), rhs.size());
  return std::max(0.0, 0.7 * (1.0 - static_cast<double>(distance) /
                                        static_cast<double>(max_size)));
}

// True when a serialized field value is a JSON array or object.
bool LooksLikeJsonStructure(std::string_view value) {
  for (char c : value) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      continue;
    }
    return c == '[' || c == '{';
  }
  return false;
}

base::flat_set<std::string> ExtractIdSet(std::string_view value) {
  base::flat_set<std::string> ids;
  std::optional<base::Value> parsed =
      base::JSONReader::Read(value, base::JSON_PARSE_RFC);
  if (!parsed) {
    return ids;
  }
  if (parsed->is_list()) {
    for (const base::Value& element : parsed->GetList()) {
      ids.insert(ValueToComparableString(element));
    }
  } else if (parsed->is_dict()) {
    for (const auto [key, element] : parsed->GetDict()) {
      ids.insert(std::string(key));
    }
  }
  return ids;
}

double JaccardSimilarity(const base::flat_set<std::string>& lhs,
                         const base::flat_set<std::string>& rhs) {
  if (lhs.empty() && rhs.empty()) {
    return 1.0;
  }
  size_t intersection = 0;
  for (const std::string& item : lhs) {
    if (rhs.contains(item)) {
      ++intersection;
    }
  }
  const size_t union_size = lhs.size() + rhs.size() - intersection;
  if (union_size == 0) {
    return 1.0;
  }
  return static_cast<double>(intersection) / static_cast<double>(union_size);
}

double OverlapCoefficient(const base::flat_set<std::string>& lhs,
                          const base::flat_set<std::string>& rhs) {
  if (lhs.empty() && rhs.empty()) {
    return 1.0;
  }
  if (lhs.empty() || rhs.empty()) {
    return 0.0;
  }
  size_t intersection = 0;
  for (const std::string& item : lhs) {
    if (rhs.contains(item)) {
      ++intersection;
    }
  }
  return static_cast<double>(intersection) /
         static_cast<double>(std::min(lhs.size(), rhs.size()));
}

bool SetContainsAll(const base::flat_set<std::string>& container,
                    const base::flat_set<std::string>& values) {
  for (const std::string& value : values) {
    if (!container.contains(value)) {
      return false;
    }
  }
  return true;
}

double FieldValueSimilarity(std::string_view lhs, std::string_view rhs) {
  if (LooksLikeJsonStructure(lhs) && LooksLikeJsonStructure(rhs)) {
    const base::flat_set<std::string> lhs_ids = ExtractIdSet(lhs);
    const base::flat_set<std::string> rhs_ids = ExtractIdSet(rhs);
    return std::max(JaccardSimilarity(lhs_ids, rhs_ids),
                    OverlapCoefficient(lhs_ids, rhs_ids));
  }
  return StringSimilarity(lhs, rhs);
}

struct DirectedFieldScore {
  double score = 0.0;
  double total = 0.0;
};

void AddFieldMapForScoring(const base::DictValue& source,
                           std::string_view prefix,
                           base::DictValue* target) {
  for (const auto [key, value] : source) {
    target->Set(std::string(prefix) + std::string(key),
                ValueToComparableString(value));
  }
}

base::DictValue CombinedFieldsForScoring(
    const base::DictValue& query_fields,
    const base::DictValue& request_body_fields) {
  base::DictValue combined;
  AddFieldMapForScoring(query_fields, "q:", &combined);
  AddFieldMapForScoring(request_body_fields, "b:", &combined);
  return combined;
}

bool IsLowValueScoringKey(std::string_view key) {
  const size_t separator = key.find(':');
  if (separator != std::string_view::npos) {
    key.remove_prefix(separator + 1);
  }
  return IsLowValueFieldName(key);
}

std::string_view FieldWithoutPrefix(std::string_view key) {
  const size_t separator = key.find(':');
  if (separator != std::string_view::npos) {
    key.remove_prefix(separator + 1);
  }
  return key;
}

std::string ComparableFieldValue(const base::DictValue& fields,
                                 std::string_view key) {
  const base::Value* value = fields.Find(key);
  if (!value) {
    return std::string();
  }
  return ValueToComparableString(*value);
}

bool NonEmptyFieldEquals(const base::DictValue& wanted,
                         const base::DictValue& candidate,
                         std::string_view key) {
  const std::string wanted_value = ComparableFieldValue(wanted, key);
  if (wanted_value.empty()) {
    return true;
  }
  const std::string candidate_value = ComparableFieldValue(candidate, key);
  return !candidate_value.empty() && candidate_value == wanted_value;
}

bool RequiredFieldEquals(const base::DictValue& wanted,
                         const base::DictValue& candidate,
                         std::string_view key) {
  const std::string wanted_value = ComparableFieldValue(wanted, key);
  const std::string candidate_value = ComparableFieldValue(candidate, key);
  return !wanted_value.empty() && !candidate_value.empty() &&
         wanted_value == candidate_value;
}

std::string FirstComparableFieldValue(
    const base::DictValue& fields,
    std::initializer_list<std::string_view> keys) {
  for (std::string_view key : keys) {
    const std::string value = ComparableFieldValue(fields, key);
    if (!value.empty()) {
      return value;
    }
  }
  return std::string();
}

bool RequiredAnyFieldEquals(const base::DictValue& wanted,
                            const base::DictValue& candidate,
                            std::initializer_list<std::string_view> keys) {
  const std::string wanted_value = FirstComparableFieldValue(wanted, keys);
  const std::string candidate_value =
      FirstComparableFieldValue(candidate, keys);
  return !wanted_value.empty() && !candidate_value.empty() &&
         wanted_value == candidate_value;
}

bool JsonIdFieldCandidateCoversRequest(const base::DictValue& wanted,
                                       const base::DictValue& candidate,
                                       std::string_view key) {
  const std::string wanted_value = ComparableFieldValue(wanted, key);
  if (wanted_value.empty()) {
    return true;
  }
  const std::string candidate_value = ComparableFieldValue(candidate, key);
  if (candidate_value.empty()) {
    return false;
  }
  if (!LooksLikeJsonStructure(wanted_value) ||
      !LooksLikeJsonStructure(candidate_value)) {
    return wanted_value == candidate_value;
  }

  const base::flat_set<std::string> wanted_ids = ExtractIdSet(wanted_value);
  if (wanted_ids.empty()) {
    return true;
  }
  const base::flat_set<std::string> candidate_ids =
      ExtractIdSet(candidate_value);
  return SetContainsAll(candidate_ids, wanted_ids);
}

base::flat_set<std::string> ExtractMessageIdSet(std::string_view value) {
  base::flat_set<std::string> ids;
  std::optional<base::Value> parsed =
      base::JSONReader::Read(value, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return ids;
  }
  for (const base::Value& item : parsed->GetList()) {
    if (!item.is_dict()) {
      continue;
    }
    const base::DictValue& item_dict = item.GetDict();
    const std::string* channel = item_dict.FindString("channel");
    const base::ListValue* timestamps = item_dict.FindList("timestamps");
    if (!channel || channel->empty() || !timestamps) {
      continue;
    }
    for (const base::Value& timestamp : *timestamps) {
      const std::string timestamp_value = ValueToComparableString(timestamp);
      if (!timestamp_value.empty()) {
        ids.insert(*channel + "\n" + timestamp_value);
      }
    }
  }
  return ids;
}

bool MessageIdsCandidateCoversRequest(const base::DictValue& wanted,
                                      const base::DictValue& candidate) {
  const std::string wanted_value =
      ComparableFieldValue(wanted, "b:message_ids");
  const std::string candidate_value =
      ComparableFieldValue(candidate, "b:message_ids");
  if (wanted_value.empty() || candidate_value.empty()) {
    return false;
  }
  const base::flat_set<std::string> wanted_ids =
      ExtractMessageIdSet(wanted_value);
  const base::flat_set<std::string> candidate_ids =
      ExtractMessageIdSet(candidate_value);
  return !wanted_ids.empty() && SetContainsAll(candidate_ids, wanted_ids);
}

double MessageIdFieldSimilarity(std::string_view lhs, std::string_view rhs) {
  const base::flat_set<std::string> lhs_ids = ExtractMessageIdSet(lhs);
  const base::flat_set<std::string> rhs_ids = ExtractMessageIdSet(rhs);
  if (lhs_ids.empty() || rhs_ids.empty()) {
    return FieldValueSimilarity(lhs, rhs);
  }
  return std::max(JaccardSimilarity(lhs_ids, rhs_ids),
                  OverlapCoefficient(lhs_ids, rhs_ids));
}

bool EndpointPathEndsWith(std::string_view endpoint_key,
                          std::string_view suffix) {
  const GURL endpoint_url{std::string(endpoint_key)};
  if (!endpoint_url.is_valid()) {
    return false;
  }
  return base::EndsWith(endpoint_url.path(), suffix,
                        base::CompareCase::SENSITIVE);
}

bool EndpointIdentityMatches(std::string_view endpoint_key,
                             const base::DictValue& wanted_fields,
                             const base::DictValue& candidate_fields) {
  if (EndpointPathEndsWith(endpoint_key, "/api/conversations.history")) {
    return RequiredFieldEquals(wanted_fields, candidate_fields, "b:channel");
  }
  if (EndpointPathEndsWith(endpoint_key, "/api/conversations.view")) {
    return RequiredFieldEquals(wanted_fields, candidate_fields, "b:channel");
  }
  if (EndpointPathEndsWith(endpoint_key, "/api/conversations.replies")) {
    return RequiredFieldEquals(wanted_fields, candidate_fields, "b:channel") &&
           RequiredAnyFieldEquals(wanted_fields, candidate_fields,
                                  {"b:ts", "b:thread_ts"});
  }
  if (EndpointPathEndsWith(endpoint_key, "/api/messages.list")) {
    return MessageIdsCandidateCoversRequest(wanted_fields, candidate_fields);
  }
  if (EndpointPathEndsWith(endpoint_key, "/channels/membership")) {
    return RequiredFieldEquals(wanted_fields, candidate_fields, "b:channel");
  }
  if (EndpointPathEndsWith(endpoint_key, "/permissions/info")) {
    return NonEmptyFieldEquals(wanted_fields, candidate_fields,
                               "b:channel_id") &&
           NonEmptyFieldEquals(wanted_fields, candidate_fields, "b:user_id");
  }
  if (EndpointPathEndsWith(endpoint_key, "/api/users.info")) {
    return NonEmptyFieldEquals(wanted_fields, candidate_fields, "b:user");
  }
  if (EndpointPathEndsWith(endpoint_key, "/api/files.info")) {
    return NonEmptyFieldEquals(wanted_fields, candidate_fields, "b:file");
  }
  if (EndpointPathEndsWith(endpoint_key, "/users/info")) {
    return JsonIdFieldCandidateCoversRequest(wanted_fields, candidate_fields,
                                             "b:updated_ids");
  }
  if (EndpointPathEndsWith(endpoint_key, "/channels/info")) {
    return JsonIdFieldCandidateCoversRequest(wanted_fields, candidate_fields,
                                             "b:updated_ids");
  }
  if (EndpointPathEndsWith(endpoint_key, "/api/conversations.genericInfo")) {
    return JsonIdFieldCandidateCoversRequest(wanted_fields, candidate_fields,
                                             "b:updated_channels");
  }
  if (EndpointPathEndsWith(endpoint_key, "/huddles/info")) {
    return JsonIdFieldCandidateCoversRequest(wanted_fields, candidate_fields,
                                             "b:channel_ids");
  }
  return true;
}

bool IsEndpointSpecificLowValueKey(std::string_view endpoint_key,
                                   std::string_view key) {
  if (EndpointPathEndsWith(endpoint_key, "/channels/membership") &&
      FieldWithoutPrefix(key) == "users") {
    return true;
  }
  return false;
}

double FieldSimilarityForEndpoint(std::string_view endpoint_key,
                                  std::string_view key,
                                  std::string_view lhs,
                                  std::string_view rhs) {
  if (EndpointPathEndsWith(endpoint_key, "/api/messages.list") &&
      FieldWithoutPrefix(key) == "message_ids") {
    return MessageIdFieldSimilarity(lhs, rhs);
  }
  return FieldValueSimilarity(lhs, rhs);
}

DirectedFieldScore ScoreDirected(const base::DictValue& wanted,
                                 const base::DictValue& candidate,
                                 std::string_view endpoint_key) {
  DirectedFieldScore result;
  for (const auto [key, value] : wanted) {
    const std::string wanted_value = ValueToComparableString(value);
    const double base_weight =
        (IsLowValueScoringKey(key) ||
         IsEndpointSpecificLowValueKey(endpoint_key, key))
            ? 0.1
            : 10.0;
    const double value_weight =
        base_weight *
        std::max<size_t>(1, std::min<size_t>(wanted_value.size(), 64));
    result.total += value_weight;

    const base::Value* candidate_value = candidate.Find(key);
    if (!candidate_value) {
      continue;
    }
    result.score +=
        value_weight *
        FieldSimilarityForEndpoint(endpoint_key, key, wanted_value,
                                   ValueToComparableString(*candidate_value));
  }
  return result;
}

double ScoreFieldSimilarity(const base::DictValue& wanted,
                            const base::DictValue& candidate,
                            std::string_view endpoint_key) {
  if (wanted.empty() && candidate.empty()) {
    return 0.0;
  }
  DirectedFieldScore forward = ScoreDirected(wanted, candidate, endpoint_key);
  DirectedFieldScore backward = ScoreDirected(candidate, wanted, endpoint_key);
  const double score = forward.score + backward.score;
  const double total = forward.total + backward.total;
  if (total == 0.0) {
    return 0.0;
  }
  return score / total;
}

void LogScoringFieldDrift(std::string_view endpoint_key,
                          const base::DictValue& wanted,
                          const base::DictValue& candidate) {
  for (const auto [key, value] : wanted) {
    if (IsLowValueScoringKey(key) ||
        IsEndpointSpecificLowValueKey(endpoint_key, key)) {
      continue;  // already down-weighted -- not a suspect
    }
    const std::string wanted_value = ValueToComparableString(value);
    const base::Value* candidate_value = candidate.Find(key);
    const double similarity =
        candidate_value ? FieldSimilarityForEndpoint(
                              endpoint_key, key, wanted_value,
                              ValueToComparableString(*candidate_value))
                        : 0.0;
    if (similarity >= 0.999) {
      continue;  // this field matched -- not the reason for the miss
    }
    const double weight =
        10.0 * std::max<size_t>(1, std::min<size_t>(wanted_value.size(), 64));
    const double penalty = weight * (1.0 - similarity);
    const std::string candidate_preview =
        candidate_value
            ? ValueToComparableString(*candidate_value).substr(0, 120)
            : "<missing>";
    LOG(ERROR) << " [WootzAuthoring] scored-drift endpoint=" << endpoint_key
               << " field=" << key
               << " present=" << (candidate_value != nullptr)
               << " similarity=" << similarity << " penalty=" << penalty
               << " req_len=" << wanted_value.size() << " req=\""
               << wanted_value.substr(0, 120) << "\" cand=\""
               << candidate_preview << "\"";
  }

  // Reverse direction: high-value fields present only in the candidate. Scoring
  // is symmetric, so a field the request lacks also drags the score down.
  for (const auto [key, value] : candidate) {
    if (IsLowValueScoringKey(key) ||
        IsEndpointSpecificLowValueKey(endpoint_key, key) || wanted.Find(key)) {
      continue;
    }
    const std::string candidate_value = ValueToComparableString(value);
    LOG(ERROR) << " [WootzAuthoring] scored-drift-extra-candidate endpoint="
               << endpoint_key << " field=" << key
               << " candidate_len=" << candidate_value.size() << " cand=\""
               << candidate_value.substr(0, 120) << "\"";
  }
}

int64_t ReadEntryOrder(const base::DictValue& dict,
                       int64_t fallback_entry_order) {
  if (const std::string* entry_order = dict.FindString("entry_order")) {
    int64_t parsed = 0;
    if (base::StringToInt64(*entry_order, &parsed)) {
      return parsed;
    }
  }
  return fallback_entry_order;
}

std::string MakeRequestKeyForUrl(std::string_view method,
                                 const GURL& url,
                                 std::string_view request_body_digest) {
  const GURL key_url = url.GetWithoutRef();
  return "sha256:" + Sha256Hex(std::string(method) + "\n" + key_url.spec() +
                               "\n" + std::string(request_body_digest));
}

std::string RequestBodyDigestForKeyInternal(
    const network::ResourceRequest& request) {
  std::optional<std::string> body = RequestBodyBytesForKey(request);
  if (!body.has_value()) {
    return WootzAuthoringStore::kNoRequestBodyDigest;
  }
  return "sha256:" + Sha256Hex(*body);
}

GURL CanonicalDocumentUrlForFallback(const GURL& url) {
  GURL canonical_url = url.GetWithoutRef();
  GURL::Replacements replacements;
  replacements.ClearQuery();
  return canonical_url.ReplaceComponents(replacements);
}

bool SamePathAllowingTrailingSlash(std::string_view lhs, std::string_view rhs) {
  if (lhs == rhs) {
    return true;
  }
  if (lhs.size() > 1 && lhs.ends_with("/") &&
      lhs.substr(0, lhs.size() - 1) == rhs) {
    return true;
  }
  if (rhs.size() > 1 && rhs.ends_with("/") &&
      rhs.substr(0, rhs.size() - 1) == lhs) {
    return true;
  }
  return false;
}

bool SameCanonicalDocumentUrl(const GURL& lhs, const GURL& rhs) {
  const GURL lhs_canonical = CanonicalDocumentUrlForFallback(lhs);
  const GURL rhs_canonical = CanonicalDocumentUrlForFallback(rhs);
  if (!lhs_canonical.is_valid() || !rhs_canonical.is_valid()) {
    return false;
  }
  return lhs_canonical.scheme() == rhs_canonical.scheme() &&
         lhs_canonical.host() == rhs_canonical.host() &&
         lhs_canonical.port() == rhs_canonical.port() &&
         SamePathAllowingTrailingSlash(lhs_canonical.path(),
                                       rhs_canonical.path());
}

bool CanUseDocumentUrlFallback(const network::ResourceRequest& request) {
  return request.method == "GET" &&
         request.resource_type ==
             static_cast<int>(blink::mojom::ResourceType::kMainFrame);
}

bool SameOriginForFallback(const GURL& lhs, const GURL& rhs) {
  return lhs.is_valid() && rhs.is_valid() && lhs.SchemeIsHTTPOrHTTPS() &&
         rhs.SchemeIsHTTPOrHTTPS() && lhs.scheme() == rhs.scheme() &&
         lhs.host() == rhs.host() && lhs.port() == rhs.port();
}

std::string NormalizeShellPathForPrefix(std::string_view path) {
  std::string normalized(path);
  while (normalized.size() > 1 && normalized.ends_with("/")) {
    normalized.pop_back();
  }
  return normalized;
}

bool IsPathUnderShellPrefix(const GURL& shell_url, const GURL& request_url) {
  const std::string shell_path = NormalizeShellPathForPrefix(shell_url.path());
  const std::string request_path =
      NormalizeShellPathForPrefix(request_url.path());
  if (shell_path.empty() || shell_path == "/") {
    return false;
  }
  return request_path == shell_path ||
         base::StartsWith(request_path, shell_path + "/",
                          base::CompareCase::SENSITIVE);
}

base::FilePath GetAuthoringRootPath(const base::FilePath& profile_path) {
#if BUILDFLAG(IS_ANDROID)
  JNIEnv* env = base::android::AttachCurrentThread();
  base::android::ScopedJavaLocalRef<jstring> j_path =
      Java_WootzAuthoringPathUtils_getAuthoringStoragePath(env);
  if (!j_path.is_null()) {
    base::FilePath path(base::android::ConvertJavaStringToUTF8(env, j_path));
    if (base::CreateDirectory(path)) {
      LOG(ERROR) << " [WootzAuthoring] Using external storage root "
                 << path.value();
      return path;
    }
    LOG(ERROR) << " [WootzAuthoring] Failed to create external authoring root "
               << path.value();
  } else {
    LOG(ERROR)
        << " [WootzAuthoring] Failed to get authoring storage path from Java";
  }
#endif
  return profile_path.Append(FILE_PATH_LITERAL("WootzAuthoring"));
}

}  // namespace

WootzAuthoringStore::WootzAuthoringStore(const base::FilePath& profile_path,
                                         std::string session_id)
    : session_id_(std::move(session_id)) {
  if (session_id_.empty()) {
    session_id_ = "default";
  }

  const base::FilePath authoring_root = GetAuthoringRootPath(profile_path);
  session_dir_ = authoring_root.Append(FILE_PATH_LITERAL("sessions"))
                     .Append(base::FilePath::FromUTF8Unsafe(session_id_));
  payloads_dir_ = session_dir_.Append(FILE_PATH_LITERAL("payloads"));
  index_path_ = session_dir_.Append(FILE_PATH_LITERAL("index.jsonl"));

  LOG(ERROR) << " [WootzAuthoring] Store init profile_path="
             << profile_path.value() << " session_dir=" << session_dir_.value()
             << " payloads_dir=" << payloads_dir_.value()
             << " index_path=" << index_path_.value();
}

WootzAuthoringStore::~WootzAuthoringStore() = default;

WootzAuthoringStore::StorageEntry::StorageEntry() = default;
WootzAuthoringStore::StorageEntry::StorageEntry(const StorageEntry&) = default;
WootzAuthoringStore::StorageEntry& WootzAuthoringStore::StorageEntry::operator=(
    const StorageEntry&) = default;
WootzAuthoringStore::StorageEntry::StorageEntry(StorageEntry&&) = default;
WootzAuthoringStore::StorageEntry& WootzAuthoringStore::StorageEntry::operator=(
    StorageEntry&&) = default;
WootzAuthoringStore::StorageEntry::~StorageEntry() = default;

WootzAuthoringStore::StorageState::StorageState() = default;
WootzAuthoringStore::StorageState::StorageState(const StorageState&) = default;
WootzAuthoringStore::StorageState& WootzAuthoringStore::StorageState::operator=(
    const StorageState&) = default;
WootzAuthoringStore::StorageState::StorageState(StorageState&&) = default;
WootzAuthoringStore::StorageState& WootzAuthoringStore::StorageState::operator=(
    StorageState&&) = default;
WootzAuthoringStore::StorageState::~StorageState() = default;

WootzAuthoringStore::StoredEntry::StoredEntry() = default;
WootzAuthoringStore::StoredEntry::StoredEntry(StoredEntry&&) = default;
WootzAuthoringStore::StoredEntry& WootzAuthoringStore::StoredEntry::operator=(
    StoredEntry&&) = default;
WootzAuthoringStore::StoredEntry::~StoredEntry() = default;

// static
std::string WootzAuthoringStore::MakeRequestKey(
    const network::ResourceRequest& request) {
  return MakeRequestKey(request.method, request.url,
                        RequestBodyDigestForKey(request));
}

// static
std::string WootzAuthoringStore::MakeRequestKey(std::string_view method,
                                                const GURL& url) {
  return MakeRequestKey(method, url, kNoRequestBodyDigest);
}

// static
std::string WootzAuthoringStore::MakeRequestKey(
    std::string_view method,
    const GURL& url,
    std::string_view request_body_digest) {
  return MakeRequestKeyForUrl(method, url, request_body_digest);
}

// static
std::string WootzAuthoringStore::MakeEndpointKey(const GURL& url) {
  return EndpointKeyForUrl(url);
}

// static
base::DictValue WootzAuthoringStore::QueryFieldsForUrl(const GURL& url) {
  return QueryFieldsForUrlInternal(url);
}

// static
base::DictValue WootzAuthoringStore::RequestBodyFieldsForKey(
    const network::ResourceRequest& request) {
  std::optional<std::string> body = RequestBodyBytesForKey(request);
  if (!body.has_value()) {
    return base::DictValue();
  }
  std::string content_type;
  if (std::optional<std::string> header_value =
          request.headers.GetHeader(net::HttpRequestHeaders::kContentType)) {
    content_type = *header_value;
  }
  return ParseRequestBodyFields(*body, content_type);
}

// static
std::string WootzAuthoringStore::MakeNormalizedRequestKey(
    std::string_view method,
    const GURL& url,
    const base::DictValue& request_body_fields,
    std::string_view request_body_digest) {
  return MakeNormalizedRequestKeyInternal(method, url, request_body_fields,
                                          request_body_digest);
}

// static
std::string WootzAuthoringStore::RequestBodyDigestForKey(
    const network::ResourceRequest& request) {
  return RequestBodyDigestForKeyInternal(request);
}

bool WootzAuthoringStore::SaveResponse(const CapturedRequestInfo& request,
                                       const CapturedResponseInfo& response,
                                       std::string_view payload) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!EnsureInitialized()) {
    return false;
  }

  const std::string payload_digest = PayloadDigest(payload);
  const base::FilePath payload_path = PayloadPathForDigest(payload_digest);
  LOG(ERROR) << " [WootzAuthoring] SaveResponse url=" << request.url
             << " payload_path=" << payload_path.value()
             << " payload_bytes=" << payload.size()
             << " payload_exists=" << base::PathExists(payload_path)
             << " session_dir_exists=" << base::PathExists(session_dir_)
             << " payloads_dir_exists=" << base::PathExists(payloads_dir_)
             << " index_exists_before_append=" << base::PathExists(index_path_);
  if (!base::PathExists(payload_path) &&
      !base::WriteFile(payload_path, payload)) {
    LOG(ERROR) << " [WootzAuthoring] Failed to write payload "
               << payload_path.value()
               << " session_dir_exists=" << base::PathExists(session_dir_)
               << " payloads_dir_exists=" << base::PathExists(payloads_dir_);
    return false;
  }

  const int64_t entry_order = next_entry_order_++;

  base::DictValue entry;
  entry.Set("record_type", "response");
  entry.Set("request_key", request.request_key);
  entry.Set("normalized_request_key", request.normalized_request_key);
  entry.Set("endpoint_key", request.endpoint_key);
  entry.Set("url", request.url.spec());
  entry.Set("canonical_url",
            CanonicalDocumentUrlForFallback(request.url).spec());
  entry.Set("method", request.method);
  entry.Set("request_body_digest", request.request_body_digest);
  entry.Set("query_fields", request.query_fields.Clone());
  entry.Set("request_body_fields", request.request_body_fields.Clone());
  entry.Set("entry_order", base::NumberToString(entry_order));
  entry.Set("request_headers", request.request_headers.Clone());
  entry.Set("resource_type", request.resource_type);
  entry.Set("request_destination", request.request_destination);
  entry.Set("factory_type", request.factory_type);
  entry.Set("request_initiator", request.request_initiator);
  entry.Set("timestamp_ms", TimeToJsonString(request.timestamp));
  entry.Set("response_status", response.status_code);
  entry.Set("response_headers", response.response_headers.Clone());
  entry.Set("raw_response_headers", response.raw_response_headers);
  entry.Set("mime_type", response.mime_type);
  entry.Set("charset", response.charset);
  entry.Set("net_error", response.net_error);
  entry.Set("payload_digest", payload_digest);
  entry.Set("payload_size", base::NumberToString(payload.size()));

  std::string json;
  if (!base::JSONWriter::Write(entry, &json)) {
    LOG(ERROR) << " [WootzAuthoring] Failed to serialize index entry for "
               << request.url;
    return false;
  }

  if (!base::AppendToFile(index_path_, json + "\n")) {
    LOG(ERROR) << " [WootzAuthoring] Failed to append index "
               << index_path_.value()
               << " parent_exists=" << base::PathExists(index_path_.DirName())
               << " parent_writable="
               << base::PathIsWritable(index_path_.DirName())
               << " index_exists_before_append="
               << base::PathExists(index_path_)
               << " payload_exists=" << base::PathExists(payload_path);
    return false;
  }

  LOG(ERROR) << " [WootzAuthoring] Appended index entry index_path="
             << index_path_.value()
             << " index_size=" << base::GetFileSize(index_path_).value_or(-1);

  StoredEntry stored;
  stored.status_code = response.status_code;
  stored.url = request.url;
  stored.method = request.method;
  stored.normalized_request_key = request.normalized_request_key;
  stored.endpoint_key = request.endpoint_key;
  stored.request_body_digest = request.request_body_digest;
  stored.query_fields = request.query_fields.Clone();
  stored.request_body_fields = request.request_body_fields.Clone();
  stored.entry_order = entry_order;
  stored.mime_type = response.mime_type;
  stored.raw_response_headers = response.raw_response_headers;
  stored.payload_digest = payload_digest;
  entries_[request.request_key] = std::move(stored);
  AddEntryToIndexes(request.request_key,
                    entries_.find(request.request_key)->second);
  index_loaded_ = true;

  LOG(ERROR) << " [WootzAuthoring] Captured " << request.method << " "
             << request.url << " payload=" << payload.size()
             << " session=" << session_id_;
  return true;
}

bool WootzAuthoringStore::SaveRedirect(const CapturedRequestInfo& request,
                                       const CapturedResponseInfo& response,
                                       const GURL& redirect_url) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!EnsureInitialized()) {
    return false;
  }

  const int64_t entry_order = next_entry_order_++;

  base::DictValue entry;
  entry.Set("record_type", "redirect");
  entry.Set("request_key", request.request_key);
  entry.Set("normalized_request_key", request.normalized_request_key);
  entry.Set("endpoint_key", request.endpoint_key);
  entry.Set("url", request.url.spec());
  entry.Set("canonical_url",
            CanonicalDocumentUrlForFallback(request.url).spec());
  entry.Set("method", request.method);
  entry.Set("request_body_digest", request.request_body_digest);
  entry.Set("query_fields", request.query_fields.Clone());
  entry.Set("request_body_fields", request.request_body_fields.Clone());
  entry.Set("entry_order", base::NumberToString(entry_order));
  entry.Set("request_headers", request.request_headers.Clone());
  entry.Set("resource_type", request.resource_type);
  entry.Set("request_destination", request.request_destination);
  entry.Set("factory_type", request.factory_type);
  entry.Set("request_initiator", request.request_initiator);
  entry.Set("timestamp_ms", TimeToJsonString(request.timestamp));
  entry.Set("response_status", response.status_code);
  entry.Set("response_headers", response.response_headers.Clone());
  entry.Set("raw_response_headers", response.raw_response_headers);
  entry.Set("mime_type", response.mime_type);
  entry.Set("charset", response.charset);
  entry.Set("net_error", response.net_error);
  entry.Set("redirect_url", redirect_url.spec());

  std::string json;
  if (!base::JSONWriter::Write(entry, &json)) {
    LOG(ERROR) << " [WootzAuthoring] Failed to serialize redirect entry for "
               << request.url << " -> " << redirect_url;
    return false;
  }

  if (!base::AppendToFile(index_path_, json + "\n")) {
    LOG(ERROR) << " [WootzAuthoring] Failed to append redirect index "
               << index_path_.value();
    return false;
  }

  StoredEntry stored;
  stored.status_code = response.status_code;
  stored.url = request.url;
  stored.method = request.method;
  stored.normalized_request_key = request.normalized_request_key;
  stored.endpoint_key = request.endpoint_key;
  stored.request_body_digest = request.request_body_digest;
  stored.query_fields = request.query_fields.Clone();
  stored.request_body_fields = request.request_body_fields.Clone();
  stored.entry_order = entry_order;
  stored.mime_type = response.mime_type;
  stored.raw_response_headers = response.raw_response_headers;
  stored.redirect_url = redirect_url.spec();
  entries_[request.request_key] = std::move(stored);
  AddEntryToIndexes(request.request_key,
                    entries_.find(request.request_key)->second);
  index_loaded_ = true;

  LOG(ERROR) << " [WootzAuthoring] Captured redirect " << request.method << " "
             << request.url << " -> " << redirect_url
             << " status=" << response.status_code
             << " session=" << session_id_;
  return true;
}

bool WootzAuthoringStore::SaveStorageState(
    const std::string& origin,
    const std::vector<StorageEntry>& local_storage) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (origin.empty() || !EnsureInitialized()) {
    return false;
  }

  base::ListValue local;
  for (const StorageEntry& entry : local_storage) {
    if (entry.key_base64.empty()) {
      continue;
    }
    base::DictValue item;
    item.Set("k", entry.key_base64);
    item.Set("v", entry.value_base64);
    local.Append(std::move(item));
  }

  base::DictValue record;
  record.Set("record_type", "storage_state");
  record.Set("origin", origin);
  record.Set("local", std::move(local));
  record.Set("timestamp_ms", TimeToJsonString(base::Time::Now()));

  std::string json;
  if (!base::JSONWriter::Write(record, &json)) {
    LOG(ERROR) << " [WootzAuthoring] Failed to serialize storage_state origin="
               << origin;
    return false;
  }
  if (!base::AppendToFile(index_path_, json + "\n")) {
    LOG(ERROR) << " [WootzAuthoring] Failed to append storage_state index "
               << index_path_.value() << " origin=" << origin;
    return false;
  }

  StorageState state;
  state.origin = origin;
  state.local_storage = local_storage;
  storage_states_by_origin_[origin] = std::move(state);
  index_loaded_ = true;

  LOG(ERROR) << " [WootzAuthoring] Captured localStorage state origin="
             << origin << " entries=" << local_storage.size()
             << " session=" << session_id_;
  return true;
}

bool WootzAuthoringStore::SaveCookieState(base::ListValue cookies) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!EnsureInitialized()) {
    return false;
  }

  const size_t cookie_count = cookies.size();
  base::DictValue record;
  record.Set("record_type", "cookie_state");
  record.Set("cookies", cookies.Clone());
  record.Set("timestamp_ms", TimeToJsonString(base::Time::Now()));

  std::string json;
  if (!base::JSONWriter::Write(record, &json)) {
    LOG(ERROR) << " [WootzAuthoring] Failed to serialize cookie_state";
    return false;
  }
  if (!base::AppendToFile(index_path_, json + "\n")) {
    LOG(ERROR) << " [WootzAuthoring] Failed to append cookie_state index "
               << index_path_.value();
    return false;
  }

  cookie_state_ = std::move(cookies);
  index_loaded_ = true;

  LOG(ERROR) << " [WootzAuthoring] Captured cookie state cookies="
             << cookie_count << " session=" << session_id_;
  return true;
}

std::vector<WootzAuthoringStore::StorageState>
WootzAuthoringStore::LoadStorageStates() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<StorageState> states;
  if (!LoadIndexIfNeeded()) {
    return states;
  }
  states.reserve(storage_states_by_origin_.size());
  for (const auto& [origin, state] : storage_states_by_origin_) {
    states.push_back(state);
  }
  return states;
}

std::optional<base::ListValue> WootzAuthoringStore::LoadCookieState() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!LoadIndexIfNeeded() || !cookie_state_.has_value()) {
    return std::nullopt;
  }
  return cookie_state_->Clone();
}

void WootzAuthoringStore::RecordSameDocumentRoute(const GURL& shell_url,
                                                  const GURL& route_url) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!route_url.is_valid() || route_url == shell_url) {
    return;
  }
  if (!EnsureInitialized()) {
    return;
  }

  const GURL canonical_route_url = CanonicalDocumentUrlForFallback(route_url);
  const GURL canonical_shell_url = CanonicalDocumentUrlForFallback(shell_url);

  // Avoid re-appending a route we already know about this session.
  auto existing = same_document_routes_.find(canonical_route_url.spec());
  if (existing != same_document_routes_.end() &&
      existing->second == canonical_shell_url.spec()) {
    return;
  }

  base::DictValue entry;
  entry.Set("record_type", "same_document_route");
  entry.Set("shell_url", canonical_shell_url.spec());
  entry.Set("route_url", canonical_route_url.spec());

  std::string json;
  if (!base::JSONWriter::Write(entry, &json)) {
    return;
  }
  if (!base::AppendToFile(index_path_, json + "\n")) {
    LOG(WARNING) << " [WootzAuthoring] Failed to append same_document_route "
                 << "route_url=" << route_url
                 << " index_path=" << index_path_.value();
    return;
  }

  same_document_routes_[canonical_route_url.spec()] =
      canonical_shell_url.spec();
  index_loaded_ = true;

  LOG(ERROR) << " [WootzAuthoring] Recorded same-document route route_url="
             << route_url << " shell_url=" << shell_url
             << " session=" << session_id_;
}

bool WootzAuthoringStore::LoadResponse(const network::ResourceRequest& request,
                                       ReplayResponse* response) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!response || !LoadIndexIfNeeded()) {
    return false;
  }

  *response = ReplayResponse();

  const std::string request_body_digest = RequestBodyDigestForKey(request);
  const base::DictValue query_fields = QueryFieldsForUrl(request.url);
  const base::DictValue request_body_fields =
      RequestBodyFieldsForKey(request);
  const std::string request_key =
      MakeRequestKey(request.method, request.url, request_body_digest);
  std::map<std::string, StoredEntry>::const_iterator it =
      entries_.find(request_key);

  if (it == entries_.end()) {
    LOG(ERROR) << " [WootzAuthoring] Replay exact miss method="
               << request.method << " url=" << request.url
               << " request_key=" << request_key
               << " request_body_digest=" << request_body_digest;

    it = FindNormalizedResponse(request, request_body_digest,
                                request_body_fields);
    if (it != entries_.end()) {
      LOG(ERROR) << " [WootzAuthoring] Replay normalized match " << request.url
                 << " -> " << it->second.url;
    }
  }

  // Static-asset query fallback (GET + static extension).Serves the
  // captured asset by path
  if (it == entries_.end()) {
    it = FindStaticAssetQueryFallback(request);
    if (it != entries_.end()) {
      LOG(ERROR) << " [WootzAuthoring] Replay static asset query fallback "
                 << request.url << " -> " << it->second.url;
    }
  }

  // Document query fallback (GET navigation). Serves the captured shell
  // by path, ignoring recovery query params or some avoidable params,
  // when the path has exactly one candidate.
  if (it == entries_.end()) {
    it = FindDocumentQueryFallback(request);
    if (it != entries_.end()) {
      LOG(ERROR) << " [WootzAuthoring] Replay document query fallback "
                 << request.url << " -> " << it->second.url;
    }
  }

  if (it == entries_.end()) {
    it = FindScoredResponse(request, query_fields, request_body_fields);
    if (it != entries_.end()) {
      LOG(ERROR) << " [WootzAuthoring] Replay scored match " << request.url
                 << " -> " << it->second.url;
    }
  }

  if (it == entries_.end()) {
    it = FindDocumentFallback(request, request_body_digest);
    if (it != entries_.end()) {
      LOG(ERROR) << " [WootzAuthoring] Replay document URL fallback "
                 << request.url << " -> " << it->second.url;
    }
  }

  if (it == entries_.end() && CanUseDocumentUrlFallback(request)) {
    const std::string route_key =
        CanonicalDocumentUrlForFallback(request.url).spec();
    auto route_it = same_document_routes_.find(route_key);
    if (route_it != same_document_routes_.end()) {
      const std::string shell_key =
          MakeRequestKey("GET", GURL(route_it->second));
      auto shell_it = entries_.find(shell_key);
      if (shell_it != entries_.end()) {
        it = shell_it;
        LOG(ERROR) << " [WootzAuthoring] Replay SPA route fallback "
                   << request.url << " -> shell " << route_it->second;
      } else {
        LOG(ERROR) << " [WootzAuthoring] SPA route maps to uncaptured shell "
                   << request.url << " -> " << route_it->second;
      }
    }
  }

  if (it == entries_.end()) {
    it = FindSpaShellPrefixFallback(request);
    if (it != entries_.end()) {
      LOG(ERROR) << " [WootzAuthoring] Replay SPA shell prefix fallback "
                 << request.url << " -> shell " << it->second.url;
    }
  }

  if (it == entries_.end()) {
    LOG(ERROR) << " [WootzAuthoring] Replay strict miss method="
               << request.method << " url=" << request.url
               << " endpoint_key=" << MakeEndpointKey(request.url);
    return false;
  }

  if (!it->second.redirect_url.empty()) {
    response->status_code = it->second.status_code;
    response->mime_type = it->second.mime_type;
    response->raw_response_headers = it->second.raw_response_headers;
    response->redirect_url = it->second.redirect_url;
    return true;
  }

  std::string payload;
  const base::FilePath payload_path =
      PayloadPathForDigest(it->second.payload_digest);
  if (!base::ReadFileToString(payload_path, &payload)) {
    LOG(WARNING) << " [WootzAuthoring] Missing payload for replay key "
                 << request_key
                 << " payload_digest=" << it->second.payload_digest
                 << " payload_path=" << payload_path.value();
    return false;
  }

  response->status_code = it->second.status_code;
  response->mime_type = it->second.mime_type;
  response->raw_response_headers = it->second.raw_response_headers;
  response->payload = std::move(payload);
  return true;
}

void WootzAuthoringStore::AddEntryToIndexes(const std::string& request_key,
                                            const StoredEntry& entry) {
  if (!entry.normalized_request_key.empty()) {
    normalized_entries_[entry.normalized_request_key] = request_key;
  }
  if (!entry.endpoint_key.empty() && !entry.method.empty()) {
    entries_by_endpoint_[EndpointIndexKey(entry.method, entry.endpoint_key)]
        .push_back(request_key);
  }
}

std::map<std::string, WootzAuthoringStore::StoredEntry>::const_iterator
WootzAuthoringStore::FindNormalizedResponse(
    const network::ResourceRequest& request,
    std::string_view request_body_digest,
    const base::DictValue& request_body_fields) const {
  const std::string normalized_request_key = MakeNormalizedRequestKey(
      request.method, request.url, request_body_fields, request_body_digest);
  auto normalized_it = normalized_entries_.find(normalized_request_key);
  if (normalized_it == normalized_entries_.end()) {
    LOG(ERROR) << " [WootzAuthoring] Replay normalized miss method="
               << request.method << " url=" << request.url
               << " normalized_request_key=" << normalized_request_key;
    return entries_.end();
  }

  auto entry_it = entries_.find(normalized_it->second);
  if (entry_it == entries_.end()) {
    LOG(ERROR) << " [WootzAuthoring] Replay normalized stale index key="
               << normalized_it->second;
    return entries_.end();
  }
  return entry_it;
}

std::map<std::string, WootzAuthoringStore::StoredEntry>::const_iterator
WootzAuthoringStore::FindSinglePathCandidate(
    const network::ResourceRequest& request) const {
  const std::string endpoint_key = MakeEndpointKey(request.url);
  const std::string endpoint_index_key =
      EndpointIndexKey(request.method, endpoint_key);
  auto endpoint_it = entries_by_endpoint_.find(endpoint_index_key);
  if (endpoint_it == entries_by_endpoint_.end()) {
    return entries_.end();
  }

  // Serve only when exactly one DISTINCT captured entry exists for this
  // (method, path); anything ambiguous falls through to scoring.
  auto result = entries_.end();
  const std::string* found_key = nullptr;
  for (const std::string& candidate_key : endpoint_it->second) {
    auto candidate_it = entries_.find(candidate_key);
    if (candidate_it == entries_.end()) {
      continue;
    }
    const StoredEntry& candidate = candidate_it->second;
    if (candidate.method != request.method ||
        candidate.endpoint_key != endpoint_key) {
      continue;
    }
    if (found_key && *found_key != candidate_key) {
      return entries_.end();  // more than one distinct candidate -> ambiguous
    }
    found_key = &candidate_key;
    result = candidate_it;
  }
  return result;
}

std::map<std::string, WootzAuthoringStore::StoredEntry>::const_iterator
WootzAuthoringStore::FindStaticAssetQueryFallback(
    const network::ResourceRequest& request) const {
  if (request.method != "GET" || !IsStaticAssetPath(request.url)) {
    return entries_.end();
  }
  return FindSinglePathCandidate(request);
}

std::map<std::string, WootzAuthoringStore::StoredEntry>::const_iterator
WootzAuthoringStore::FindDocumentQueryFallback(
    const network::ResourceRequest& request) const {
  if (request.method != "GET" ||
      request.destination != network::mojom::RequestDestination::kDocument) {
    return entries_.end();
  }
  return FindSinglePathCandidate(request);
}

std::map<std::string, WootzAuthoringStore::StoredEntry>::const_iterator
WootzAuthoringStore::FindScoredResponse(
    const network::ResourceRequest& request,
    const base::DictValue& query_fields,
    const base::DictValue& request_body_fields) const {
  const std::string endpoint_key = MakeEndpointKey(request.url);
  const std::string endpoint_index_key =
      EndpointIndexKey(request.method, endpoint_key);
  auto endpoint_it = entries_by_endpoint_.find(endpoint_index_key);
  if (endpoint_it == entries_by_endpoint_.end()) {
    LOG(ERROR) << " [WootzAuthoring] Replay scored miss no_endpoint_candidates "
               << request.method << " " << endpoint_key;
    return entries_.end();
  }

  const base::DictValue wanted_fields =
      CombinedFieldsForScoring(query_fields, request_body_fields);
  if (wanted_fields.empty()) {
    LOG(ERROR) << " [WootzAuthoring] Replay scored skipped no_fields method="
               << request.method << " endpoint_key=" << endpoint_key
               << " candidate_count=" << endpoint_it->second.size();
    return entries_.end();
  }

  auto best_it = entries_.end();
  double best_score = -std::numeric_limits<double>::infinity();
  size_t candidate_count = 0;
  for (const std::string& candidate_key : endpoint_it->second) {
    auto candidate_it = entries_.find(candidate_key);
    if (candidate_it == entries_.end()) {
      continue;
    }
    const StoredEntry& candidate = candidate_it->second;
    if (candidate.method != request.method ||
        candidate.endpoint_key != endpoint_key) {
      continue;
    }

    const base::DictValue candidate_fields = CombinedFieldsForScoring(
        candidate.query_fields, candidate.request_body_fields);
    if (!EndpointIdentityMatches(endpoint_key, wanted_fields,
                                 candidate_fields)) {
      continue;
    }

    ++candidate_count;
    const double score =
        ScoreFieldSimilarity(wanted_fields, candidate_fields, endpoint_key);
    if (score > best_score ||
        (score == best_score && best_it != entries_.end() &&
         candidate.entry_order > best_it->second.entry_order)) {
      best_score = score;
      best_it = candidate_it;
    }
  }

  LOG(ERROR) << " [WootzAuthoring] Replay scored candidates method="
             << request.method << " endpoint_key=" << endpoint_key
             << " candidate_count=" << candidate_count
             << " best_score=" << best_score
             << " threshold=" << kMinScoredReplayMatch << " best_url="
             << (best_it == entries_.end() ? GURL() : best_it->second.url);

  // Just for Loggings
  if (best_it != entries_.end() && best_score < kMinScoredReplayMatch) {
    const base::DictValue best_fields = CombinedFieldsForScoring(
        best_it->second.query_fields, best_it->second.request_body_fields);
    LogScoringFieldDrift(endpoint_key, wanted_fields, best_fields);
  }

  if (best_it == entries_.end() || best_score < kMinScoredReplayMatch) {
    return entries_.end();
  }

  const base::DictValue best_fields = CombinedFieldsForScoring(
      best_it->second.query_fields, best_it->second.request_body_fields);
  LOG(ERROR) << " [WootzAuthoring] Replay scored accepted endpoint="
             << endpoint_key << " score=" << best_score << " req_channel="
             << ComparableFieldValue(wanted_fields, "b:channel")
             << " cand_channel="
             << ComparableFieldValue(best_fields, "b:channel")
             << " req_ts=" << ComparableFieldValue(wanted_fields, "b:ts")
             << " cand_ts=" << ComparableFieldValue(best_fields, "b:ts")
             << " payload_digest=" << best_it->second.payload_digest;
  return best_it;
}

std::map<std::string, WootzAuthoringStore::StoredEntry>::const_iterator
WootzAuthoringStore::FindDocumentFallback(
    const network::ResourceRequest& request,
    std::string_view request_body_digest) const {
  if (!CanUseDocumentUrlFallback(request)) {
    return entries_.end();
  }

  auto best_it = entries_.end();
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    const StoredEntry& entry = it->second;
    if (!entry.redirect_url.empty() || entry.method != request.method ||
        entry.request_body_digest != request_body_digest ||
        !SameCanonicalDocumentUrl(entry.url, request.url)) {
      continue;
    }

    if (best_it == entries_.end() ||
        entry.entry_order > best_it->second.entry_order) {
      best_it = it;
    }
  }

  return best_it;
}

std::map<std::string, WootzAuthoringStore::StoredEntry>::const_iterator
WootzAuthoringStore::FindSpaShellPrefixFallback(
    const network::ResourceRequest& request) const {
  if (!CanUseDocumentUrlFallback(request) || IsStaticAssetPath(request.url)) {
    return entries_.end();
  }

  auto best_it = entries_.end();
  size_t best_path_length = 0;
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    const StoredEntry& entry = it->second;
    const bool is_html_shell =
        entry.status_code >= 200 && entry.status_code < 300 &&
        entry.redirect_url.empty() && entry.method == "GET" &&
        entry.request_body_digest == kNoRequestBodyDigest &&
        base::StartsWith(entry.mime_type, "text/html",
                         base::CompareCase::INSENSITIVE_ASCII) &&
        !IsStaticAssetPath(entry.url);
    if (!is_html_shell || !SameOriginForFallback(entry.url, request.url) ||
        !IsPathUnderShellPrefix(entry.url, request.url)) {
      continue;
    }

    const size_t path_length =
        NormalizeShellPathForPrefix(entry.url.path()).size();
    if (best_it == entries_.end() || path_length > best_path_length ||
        (path_length == best_path_length &&
         entry.entry_order > best_it->second.entry_order)) {
      best_it = it;
      best_path_length = path_length;
    }
  }

  return best_it;
}

bool WootzAuthoringStore::EnsureInitialized() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (initialized_) {
    return true;
  }
  LOG(ERROR) << " [WootzAuthoringDEV] EnsureInitialized session_dir="
             << session_dir_.value()
             << " payloads_dir=" << payloads_dir_.value()
             << " session_dir_exists_before=" << base::PathExists(session_dir_)
             << " payloads_dir_exists_before="
             << base::PathExists(payloads_dir_);
  if (!base::CreateDirectory(payloads_dir_)) {
    LOG(ERROR) << " [WootzAuthoringDEV] Failed to create "
               << payloads_dir_.value() << " session_parent_exists="
               << base::PathExists(session_dir_.DirName());
    return false;
  }
  LOG(ERROR) << " [WootzAuthoringDEV] EnsureInitialized created payloads_dir="
             << payloads_dir_.value()
             << " session_dir_exists_after=" << base::PathExists(session_dir_)
             << " payloads_dir_exists_after=" << base::PathExists(payloads_dir_)
             << " payloads_dir_writable="
             << base::PathIsWritable(payloads_dir_);

  if (!base::PathExists(index_path_)) {
    const int bytes_written = base::WriteFile(index_path_, std::string());
    if (bytes_written < 0) {
      LOG(ERROR) << " [WootzAuthoring] Failed to create empty index file "
                 << index_path_.value()
                 << " parent_exists=" << base::PathExists(index_path_.DirName())
                 << " parent_writable="
                 << base::PathIsWritable(index_path_.DirName());
      return false;
    }
    LOG(ERROR) << " [WootzAuthoring] Created empty index file "
               << index_path_.value();
  }

  initialized_ = true;
  return true;
}

bool WootzAuthoringStore::LoadIndexIfNeeded() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (index_loaded_) {
    return true;
  }
  if (!EnsureInitialized()) {
    return false;
  }
  if (!base::PathExists(index_path_)) {
    index_loaded_ = true;
    return true;
  }

  std::string index_contents;
  if (!base::ReadFileToString(index_path_, &index_contents)) {
    LOG(ERROR) << " [WootzAuthoring] Failed to read index "
               << index_path_.value();
    return false;
  }

  for (std::string_view line :
       base::SplitStringPiece(index_contents, "\n", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)) {
    std::optional<base::Value> value =
        base::JSONReader::Read(line, base::JSON_PARSE_RFC);
    if (!value || !value->is_dict()) {
      continue;
    }

    const base::DictValue& dict = value->GetDict();

    if (const std::string* record_type = dict.FindString("record_type")) {
      if (*record_type == "storage_state") {
        const std::string* origin = dict.FindString("origin");
        const base::ListValue* local = dict.FindList("local");
        if (origin && local && !origin->empty()) {
          StorageState state;
          state.origin = *origin;
          for (const base::Value& item_value : *local) {
            if (!item_value.is_dict()) {
              continue;
            }
            const base::DictValue& item = item_value.GetDict();
            const std::string* key = item.FindString("k");
            const std::string* stored_value = item.FindString("v");
            if (!key || !stored_value || key->empty()) {
              continue;
            }
            StorageEntry entry;
            entry.key_base64 = *key;
            entry.value_base64 = *stored_value;
            state.local_storage.push_back(std::move(entry));
          }
          storage_states_by_origin_[*origin] = std::move(state);
        }
        continue;
      }

      if (*record_type == "cookie_state") {
        if (const base::ListValue* cookies = dict.FindList("cookies")) {
          cookie_state_ = cookies->Clone();
        }
        continue;
      }
    }

    // Same-document (SPA) route records map a deep-link URL back to the shell
    // it was reached from. They have no request_key/payload; handle separately.
    if (const std::string* record_type = dict.FindString("record_type");
        record_type && *record_type == "same_document_route") {
      const std::string* route_url = dict.FindString("route_url");
      const std::string* shell_url = dict.FindString("shell_url");
      if (route_url && shell_url && !route_url->empty()) {
        const GURL canonical_route_url =
            CanonicalDocumentUrlForFallback(GURL(*route_url));
        const GURL canonical_shell_url =
            CanonicalDocumentUrlForFallback(GURL(*shell_url));
        same_document_routes_[canonical_route_url.spec()] =
            canonical_shell_url.spec();
      }
      continue;
    }

    const std::string* request_key = dict.FindString("request_key");
    const std::string* payload_digest = dict.FindString("payload_digest");
    const std::string* redirect_url = dict.FindString("redirect_url");
    if (!request_key || (!payload_digest && !redirect_url)) {
      continue;
    }

    StoredEntry entry;
    entry.status_code = dict.FindInt("response_status").value_or(0);
    if (const std::string* url = dict.FindString("url")) {
      entry.url = GURL(*url);
    }
    if (const std::string* method = dict.FindString("method")) {
      entry.method = *method;
    }
    if (const std::string* request_body_digest =
            dict.FindString("request_body_digest")) {
      entry.request_body_digest = *request_body_digest;
    } else {
      entry.request_body_digest = kNoRequestBodyDigest;
    }
    if (const base::DictValue* query_fields = dict.FindDict("query_fields")) {
      entry.query_fields = query_fields->Clone();
    } else {
      entry.query_fields = QueryFieldsForUrl(entry.url);
    }
    if (const base::DictValue* request_body_fields =
            dict.FindDict("request_body_fields")) {
      entry.request_body_fields = request_body_fields->Clone();
    }
    if (const std::string* endpoint_key = dict.FindString("endpoint_key")) {
      entry.endpoint_key = *endpoint_key;
    } else {
      entry.endpoint_key = MakeEndpointKey(entry.url);
    }
    if (const std::string* normalized_request_key =
            dict.FindString("normalized_request_key")) {
      entry.normalized_request_key = *normalized_request_key;
    } else {
      entry.normalized_request_key = MakeNormalizedRequestKey(
          entry.method, entry.url, entry.request_body_fields,
          entry.request_body_digest);
    }
    const int64_t fallback_entry_order = next_entry_order_++;
    entry.entry_order = ReadEntryOrder(dict, fallback_entry_order);
    next_entry_order_ = std::max(next_entry_order_, entry.entry_order + 1);
    if (const std::string* mime_type = dict.FindString("mime_type")) {
      entry.mime_type = *mime_type;
    }
    if (const std::string* raw_headers =
            dict.FindString("raw_response_headers")) {
      entry.raw_response_headers = *raw_headers;
    }
    if (payload_digest) {
      entry.payload_digest = *payload_digest;
    }
    if (redirect_url) {
      entry.redirect_url = *redirect_url;
    }

    entries_[*request_key] = std::move(entry);
    AddEntryToIndexes(*request_key, entries_.find(*request_key)->second);
  }

  index_loaded_ = true;
  LOG(ERROR) << " [WootzAuthoring] Loaded " << entries_.size()
             << " replay entries for session=" << session_id_;
  return true;
}

base::FilePath WootzAuthoringStore::PayloadPathForDigest(
    const std::string& payload_digest) const {
  return payloads_dir_.Append(base::FilePath::FromUTF8Unsafe(payload_digest));
}

}  // namespace wootz_authoring
