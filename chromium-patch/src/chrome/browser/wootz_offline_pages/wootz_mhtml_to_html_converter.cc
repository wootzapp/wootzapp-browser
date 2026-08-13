// Copyright 2024 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wootz_offline_pages/wootz_mhtml_to_html_converter.h"

#include <string>
#include <vector>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/hash/sha1.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"

namespace wootz_offline_pages {

MHTMLPart::MHTMLPart() = default;
MHTMLPart::~MHTMLPart() = default;
MHTMLPart::MHTMLPart(const MHTMLPart&) = default;
MHTMLPart& MHTMLPart::operator=(const MHTMLPart&) = default;
MHTMLPart::MHTMLPart(MHTMLPart&&) = default;
MHTMLPart& MHTMLPart::operator=(MHTMLPart&&) = default;

namespace {

// Helper function to trim whitespace
std::string Trim(const std::string& str) {
  size_t start = str.find_first_not_of(" \t\r\n");
  if (start == std::string::npos)
    return "";
  size_t end = str.find_last_not_of(" \t\r\n");
  return str.substr(start, end - start + 1);
}

// Helper function to find header value
std::string FindHeader(const std::string& headers, const std::string& name) {
  size_t pos = headers.find(name + ":");
  if (pos == std::string::npos)
    return "";
  
  size_t start = pos + name.length() + 1;
  size_t end = headers.find('\n', start);
  if (end == std::string::npos)
    end = headers.length();
  
  std::string value = headers.substr(start, end - start);
  return Trim(value);
}

}  // namespace

// static
bool WootzMHTMLToHTMLConverter::Convert(const base::FilePath& mhtml_path,
                                        const base::FilePath& html_path) {
  LOG(INFO) << " Converting MHTML to HTML: " << mhtml_path.value();
  
  // Read MHTML file
  std::string mhtml_content;
  if (!base::ReadFileToString(mhtml_path, &mhtml_content)) {
    LOG(ERROR) << " Failed to read MHTML file: " << mhtml_path.value();
    return false;
  }
  
  // Parse MHTML parts
  std::vector<MHTMLPart> parts = ParseMHTML(mhtml_content);
  if (parts.empty()) {
    LOG(ERROR) << " Failed to parse MHTML, no parts found";
    return false;
  }
  
  // LOG(INFO) << " Parsed " << parts.size() << " MHTML parts";
  
  // Build single HTML file
  std::string html = BuildHTML(parts);
  if (html.empty()) {
    LOG(ERROR) << " Failed to build HTML from MHTML parts";
    return false;
  }
  
  // Write HTML file TEMPORARILY (will be deleted after merging into flow file)
  if (!base::WriteFile(html_path, html)) {
    LOG(ERROR) << " Failed to write temporary HTML file: " << html_path.value();
    return false;
  }
  
  LOG(INFO) << " Saved temporary HTML: " << html_path.value()
            << " (size: " << html.length() << " bytes) - will be merged then deleted";
  return true;
}

// static
std::string WootzMHTMLToHTMLConverter::ExtractBoundary(
    const std::string& mhtml_content) {
  // Find boundary in Content-Type header
  size_t boundary_pos = mhtml_content.find("boundary=\"");
  if (boundary_pos == std::string::npos) {
    // Try without quotes
    boundary_pos = mhtml_content.find("boundary=");
    if (boundary_pos == std::string::npos)
      return "";
    
    size_t start = boundary_pos + 9;  // length of "boundary="
    size_t end = mhtml_content.find_first_of("\r\n;", start);
    if (end == std::string::npos)
      end = mhtml_content.length();
    return "--" + Trim(mhtml_content.substr(start, end - start));
  }
  
  size_t start = boundary_pos + 10;  // length of "boundary=\""
  size_t end = mhtml_content.find('"', start);
  if (end == std::string::npos)
    return "";
  
  return "--" + mhtml_content.substr(start, end - start);
}

// static
std::vector<MHTMLPart> WootzMHTMLToHTMLConverter::ParseMHTML(
    const std::string& mhtml_content) {
  std::vector<MHTMLPart> parts;
  
  // Extract boundary
  std::string boundary = ExtractBoundary(mhtml_content);
  if (boundary.empty()) {
    LOG(ERROR) << " Failed to extract MHTML boundary";
    return parts;
  }
  
  LOG(INFO) << " MHTML boundary: " << boundary;
  
  // Split by boundary
  size_t pos = 0;
  while ((pos = mhtml_content.find(boundary, pos)) != std::string::npos) {
    // Skip the boundary line
    pos += boundary.length();
    
    // Check for end boundary
    if (pos + 2 < mhtml_content.length() &&
        mhtml_content[pos] == '-' && mhtml_content[pos + 1] == '-') {
      break;  // End of MHTML
    }
    
    // Skip newlines after boundary
    while (pos < mhtml_content.length() &&
           (mhtml_content[pos] == '\r' || mhtml_content[pos] == '\n')) {
      pos++;
    }
    
    // Find next boundary or end
    size_t next_boundary = mhtml_content.find(boundary, pos);
    if (next_boundary == std::string::npos)
      next_boundary = mhtml_content.length();
    
    std::string part_content = mhtml_content.substr(pos, next_boundary - pos);
    
    // Split headers and body (separated by double newline)
    size_t body_start = part_content.find("\r\n\r\n");
    if (body_start == std::string::npos)
      body_start = part_content.find("\n\n");
    
    if (body_start == std::string::npos) {
      pos = next_boundary;
      continue;
    }
    
    std::string headers = part_content.substr(0, body_start);
    std::string body = part_content.substr(body_start + 4);  // Skip \r\n\r\n
    
    // Parse headers
    MHTMLPart part;
    part.content_type = FindHeader(headers, "Content-Type");
    part.content_location = FindHeader(headers, "Content-Location");
    part.content_id = FindHeader(headers, "Content-ID");
    part.transfer_encoding = FindHeader(headers, "Content-Transfer-Encoding");
    
    // Decode body based on transfer encoding
    if (part.transfer_encoding == "quoted-printable") {
      part.data = DecodeQuotedPrintable(body);
    } else if (part.transfer_encoding == "base64") {
      part.data = DecodeBase64(body);
    } else {
      part.data = body;
    }
    
    if (!part.content_type.empty()) {
      parts.push_back(part);
      // LOG(INFO) << " Parsed part - Type: " << part.content_type
      //           << ", Size: " << part.data.length() << " bytes";
    }
    
    pos = next_boundary;
  }
  
  return parts;
}

// static
std::string WootzMHTMLToHTMLConverter::DecodeQuotedPrintable(
    const std::string& encoded) {
  std::string decoded;
  decoded.reserve(encoded.length());
  
  for (size_t i = 0; i < encoded.length(); i++) {
    if (encoded[i] == '=') {
      if (i + 2 < encoded.length()) {
        // Check for soft line break (= at end of line)
        if (encoded[i + 1] == '\r' || encoded[i + 1] == '\n') {
          // Skip soft line break
          if (encoded[i + 1] == '\r' && i + 2 < encoded.length() &&
              encoded[i + 2] == '\n') {
            i += 2;
          } else {
            i += 1;
          }
          continue;
        }
        
        // Decode =XX hex sequence
        std::string hex_str(1, encoded[i + 1]);
        hex_str += encoded[i + 2];
        int value;
        if (base::HexStringToInt(hex_str, &value)) {
          decoded += static_cast<char>(value);
          i += 2;
        } else {
          decoded += encoded[i];
        }
      } else {
        decoded += encoded[i];
      }
    } else {
      decoded += encoded[i];
    }
  }
  
  return decoded;
}

// static
std::string WootzMHTMLToHTMLConverter::DecodeBase64(const std::string& encoded) {
  std::string cleaned;
  for (char c : encoded) {
    if (c != '\r' && c != '\n' && c != ' ' && c != '\t') {
      cleaned += c;
    }
  }
  
  std::string decoded;
  if (!base::Base64Decode(cleaned, &decoded)) {
    LOG(WARNING) << " Failed to decode base64, using original";
    return encoded;
  }
  
  return decoded;
}

// static
std::string WootzMHTMLToHTMLConverter::CreateDataURI(
    const std::string& content_type,
    const std::string& data,
    bool is_base64) {
  std::string uri = "data:" + content_type;
  
  // Always encode to base64, since data might be binary
  // (even if it was originally base64, we decoded it in ParseMHTML)
  std::string encoded = base::Base64Encode(data);
  uri += ";base64," + encoded;
  
  return uri;
}

// static
std::string WootzMHTMLToHTMLConverter::URLToFileName(const std::string& url) {
  // Compute SHA1 hash of the URL
  std::string hash = base::SHA1HashString(url);
  std::string hex_hash = base::HexEncode(hash);
  return "wootz_offline_" + hex_hash + ".html";
}

// static
void WootzMHTMLToHTMLConverter::RewriteLinks(std::string& html) {
  LOG(INFO) << " Rewriting links in HTML for offline navigation";
  
  size_t pos = 0;
  int rewritten_count = 0;
  
  // Find and replace all href attributes
  while ((pos = html.find("href=\"", pos)) != std::string::npos) {
    size_t url_start = pos + 6;  // After href="
    size_t url_end = html.find('"', url_start);
    
    if (url_end == std::string::npos) {
      pos = url_start;
      continue;
    }
    
    std::string url = html.substr(url_start, url_end - url_start);
    
    // Only process http and https URLs
    if (url.find("http://") == 0 || url.find("https://") == 0) {
      // Skip anchor links (they contain #)
      size_t hash_pos = url.find('#');
      std::string base_url = url;
      std::string fragment;
      
      if (hash_pos != std::string::npos) {
        base_url = url.substr(0, hash_pos);
        fragment = url.substr(hash_pos);  // Keep the # and fragment
      }
      
      // Convert URL to file name
      std::string new_href = URLToFileName(base_url);
      
      // Add fragment back if it exists
      if (!fragment.empty()) {
        new_href += fragment;
      }
      
      // Replace the URL in the HTML
      html.replace(url_start, url_end - url_start, new_href);
      
      rewritten_count++;
      pos = url_start + new_href.length();
    } else {
      pos = url_end;
    }
  }
  
  LOG(INFO) << " Rewrote " << rewritten_count << " links for offline navigation";
}

std::string WootzMHTMLToHTMLConverter::GetURLWithoutQuery(const std::string& url) {
  size_t query_pos = url.find('?');
  if (query_pos != std::string::npos) {
    return url.substr(0, query_pos);
  }
  return url;
}

// Replace the ReplaceImageURLsWithQueryVariants function with this enhanced version:
void WootzMHTMLToHTMLConverter::ReplaceImageURLsWithQueryVariants(
    std::string& html,
    const std::string& content_location,
    const std::string& data_uri) {
  
  std::string base_url = GetURLWithoutQuery(content_location);
  
  LOG(INFO) << "[IMAGE REPLACE] Looking for base URL: " << base_url;
  
  int replaced_count = 0;
  
  // 1. Replace <img src="..."> 
  size_t pos = 0;
  while ((pos = html.find("<img", pos)) != std::string::npos) {
    size_t img_end = html.find(">", pos);
    if (img_end == std::string::npos) break;
    
    size_t src_pos = html.find("src=\"", pos);
    if (src_pos != std::string::npos && src_pos < img_end) {
      size_t url_start = src_pos + 5;
      size_t url_end = html.find("\"", url_start);
      
      if (url_end != std::string::npos && url_end < img_end) {
        std::string current_url = html.substr(url_start, url_end - url_start);
        std::string current_base = GetURLWithoutQuery(current_url);
        
        if (current_base == base_url) {
          html.replace(url_start, current_url.length(), data_uri);
          replaced_count++;
          LOG(INFO) << "[IMAGE REPLACE] Replaced <img src>: " << current_url;
          pos = url_start + data_uri.length();
          continue;
        }
      }
    }
    pos = img_end;
  }
  
  // 2. Replace <image href="..."> (SVG)
  pos = 0;
  while ((pos = html.find("<image", pos)) != std::string::npos) {
    size_t img_end = html.find(">", pos);
    if (img_end == std::string::npos) break;
    
    // SVG <image> uses href or xlink:href
    size_t href_pos = html.find("href=\"", pos);
    if (href_pos != std::string::npos && href_pos < img_end) {
      size_t url_start = href_pos + 6;
      size_t url_end = html.find("\"", url_start);
      
      if (url_end != std::string::npos && url_end < img_end) {
        std::string current_url = html.substr(url_start, url_end - url_start);
        std::string current_base = GetURLWithoutQuery(current_url);
        
        if (current_base == base_url) {
          html.replace(url_start, current_url.length(), data_uri);
          replaced_count++;
          LOG(INFO) << "[IMAGE REPLACE] Replaced <image href>: " << current_url;
          pos = url_start + data_uri.length();
          continue;
        }
      }
    }
    pos = img_end;
  }
  
  // 3. Replace <use href="..."> (SVG symbols)
  pos = 0;
  while ((pos = html.find("<use", pos)) != std::string::npos) {
    size_t use_end = html.find(">", pos);
    if (use_end == std::string::npos) break;
    
    size_t href_pos = html.find("href=\"", pos);
    if (href_pos != std::string::npos && href_pos < use_end) {
      size_t url_start = href_pos + 6;
      size_t url_end = html.find("\"", url_start);
      
      if (url_end != std::string::npos && url_end < use_end) {
        std::string current_url = html.substr(url_start, url_end - url_start);
        
        // Skip internal references (e.g., #icon-logo)
        if (current_url[0] == '#') {
          pos = use_end;
          continue;
        }
        
        std::string current_base = GetURLWithoutQuery(current_url);
        
        if (current_base == base_url) {
          html.replace(url_start, current_url.length(), data_uri);
          replaced_count++;
          LOG(INFO) << "[IMAGE REPLACE] Replaced <use href>: " << current_url;
          pos = url_start + data_uri.length();
          continue;
        }
      }
    }
    pos = use_end;
  }
  
  // 4. Replace CSS background images: url('...')
  pos = 0;
  while ((pos = html.find("url(", pos)) != std::string::npos) {
    size_t url_start = pos + 4;
    char quote = '\0';
    
    // Check for quotes
    if (url_start < html.length()) {
      if (html[url_start] == '"' || html[url_start] == '\'') {
        quote = html[url_start];
        url_start++;
      }
    }
    
    size_t url_end;
    if (quote) {
      url_end = html.find(quote, url_start);
    } else {
      url_end = html.find(")", url_start);
    }
    
    if (url_end != std::string::npos) {
      std::string current_url = html.substr(url_start, url_end - url_start);
      
      // Skip data URIs and internal references
      if (current_url.find("data:") == 0 || current_url[0] == '#') {
        pos = url_end;
        continue;
      }
      
      std::string current_base = GetURLWithoutQuery(current_url);
      
      if (current_base == base_url) {
        html.replace(url_start, current_url.length(), data_uri);
        replaced_count++;
        LOG(INFO) << "[IMAGE REPLACE] Replaced CSS url(): " << current_url;
        pos = url_start + data_uri.length();
        continue;
      }
    }
    
    pos = url_end != std::string::npos ? url_end : pos + 4;
  }
  
  // 5. Replace srcset attribute (responsive images)
  pos = 0;
  while ((pos = html.find("srcset=\"", pos)) != std::string::npos) {
    size_t srcset_start = pos + 8;
    size_t srcset_end = html.find("\"", srcset_start);
    
    if (srcset_end != std::string::npos) {
      std::string srcset = html.substr(srcset_start, srcset_end - srcset_start);
      std::string original_srcset = srcset;
      
      // srcset can have multiple URLs: "url1 1x, url2 2x"
      size_t search_pos = 0;
      while ((search_pos = srcset.find("http", search_pos)) != std::string::npos) {
        size_t url_end_pos = srcset.find_first_of(" ,", search_pos);
        if (url_end_pos == std::string::npos) {
          url_end_pos = srcset.length();
        }
        
        std::string current_url = srcset.substr(search_pos, url_end_pos - search_pos);
        std::string current_base = GetURLWithoutQuery(current_url);
        
        if (current_base == base_url) {
          // Replace this URL in srcset
          srcset.replace(search_pos, current_url.length(), data_uri);
          replaced_count++;
          LOG(INFO) << "[IMAGE REPLACE] Replaced srcset URL: " << current_url;
          search_pos += data_uri.length();
        } else {
          search_pos = url_end_pos;
        }
      }
      
      // If we made changes, replace the entire srcset
      if (srcset != original_srcset) {
        html.replace(srcset_start, original_srcset.length(), srcset);
      }
      
      pos = srcset_end;
    } else {
      break;
    }
  }
  
  if (replaced_count > 0) {
    LOG(INFO) << "[IMAGE REPLACE] Successfully replaced " << replaced_count << " image reference(s)";
  } else {
    LOG(WARNING) << "[IMAGE REPLACE] No matching images found for: " << base_url;
    
    // Fallback: Try exact URL match
    int exact_count = 0;
    pos = 0;
    while ((pos = html.find(content_location, pos)) != std::string::npos) {
      // Check if this is in an image context (src=, href=, url()
      bool is_image_context = false;
      
      // Look backwards for context
      if (pos >= 5) {
        std::string before = html.substr(pos - 5, 5);
        if (before == "src=\"" || before.find("href=\"") != std::string::npos) {
          is_image_context = true;
        }
      }
      
      if (pos >= 4) {
        std::string before = html.substr(pos - 4, 4);
        if (before == "url(") {
          is_image_context = true;
        }
      }
      
      if (is_image_context) {
        html.replace(pos, content_location.length(), data_uri);
        exact_count++;
        LOG(INFO) << "[IMAGE REPLACE] Replaced via exact match: " << content_location;
        pos += data_uri.length();
      } else {
        pos++;
      }
    }
    
    if (exact_count > 0) {
      LOG(INFO) << "[IMAGE REPLACE] Replaced " << exact_count << " image(s) via exact match";
    }
  }
}

// static
std::string WootzMHTMLToHTMLConverter::BuildHTML(
    const std::vector<MHTMLPart>& parts) {
  if (parts.empty()) {
    LOG(ERROR) << " No parts to build HTML from";
    return "";
  }
  
  // First part should be HTML
  if (parts[0].content_type.find("text/html") == std::string::npos) {
    LOG(ERROR) << " First part is not HTML: " << parts[0].content_type;
    return "";
  }
  
  std::string html = parts[0].data;
  LOG(INFO) << " Base HTML size: " << html.length() << " bytes";
  
  // Process remaining parts
  for (size_t i = 1; i < parts.size(); i++) {
    const MHTMLPart& part = parts[i];
    
    if (part.content_type.find("text/css") != std::string::npos) {
      // Inline CSS
      std::string style_tag = "<style>\n" + part.data + "\n</style>\n";
      
      // Try to insert before </head>
      size_t head_end = html.find("</head>");
      if (head_end != std::string::npos) {
        html.insert(head_end, style_tag);
      } else {
        // If no </head>, insert at the beginning after <html>
        size_t html_start = html.find("<html");
        if (html_start != std::string::npos) {
          size_t insert_pos = html.find('>', html_start);
          if (insert_pos != std::string::npos) {
            html.insert(insert_pos + 1, "\n<head>\n" + style_tag + "</head>\n");
          }
        }
      }
      
      // Replace cid: references
      if (!part.content_id.empty()) {
        std::string cid = part.content_id;
        // Remove < and > if present
        if (cid.front() == '<')
          cid = cid.substr(1);
        if (cid.back() == '>')
          cid = cid.substr(0, cid.length() - 1);
        
        // This CSS is now inline, so remove the link reference
        std::string link_pattern = "href=\"cid:" + cid + "\"";
        size_t link_pos = html.find(link_pattern);
        if (link_pos != std::string::npos) {
          // Find and remove the entire <link> tag
          size_t link_start = html.rfind("<link", link_pos);
          if (link_start != std::string::npos) {
            size_t link_end = html.find(">", link_pos);
            if (link_end != std::string::npos) {
              html.erase(link_start, link_end - link_start + 1);
            }
          }
        }
      }
      
      // LOG(INFO) << " Inlined CSS part " << i;
      
    } else if (part.content_type.find("image/") != std::string::npos) {
      // Convert images to data URIs
      // Images are already base64 encoded in MHTML
      bool is_base64 = (part.transfer_encoding == "base64");
      std::string data_uri = CreateDataURI(part.content_type, part.data, is_base64);
      
      // Replace references
      if (!part.content_location.empty()) {
        // Replace src="URL" with src="data:..."
        ReplaceImageURLsWithQueryVariants(html, part.content_location, data_uri);
        // base::ReplaceSubstringsAfterOffset(&html, 0, part.content_location, data_uri);
        // LOG(INFO) << " Replaced image URL: " << part.content_location;
      }
      
      if (!part.content_id.empty()) {
        std::string cid = part.content_id;
        if (cid.front() == '<')
          cid = cid.substr(1);
        if (cid.back() == '>')
          cid = cid.substr(0, cid.length() - 1);
        
        std::string cid_ref = "cid:" + cid;
        base::ReplaceSubstringsAfterOffset(&html, 0, cid_ref, data_uri);
        // LOG(INFO) << " Replaced image CID: " << cid;
      }
    }
  }
  
  LOG(INFO) << " Final HTML size: " << html.length() << " bytes";
  
  // Add scroll fixes and viewport for better mobile experience
  AddScrollFixCSS(html);
  AddViewportMetaTag(html);
  
  LOG(INFO) << " Individual page saved with scroll fixes and viewport applied";
  
  return html;
}

// static
void WootzMHTMLToHTMLConverter::AddScrollFixCSS(std::string& html) {
  std::string scroll_fix_css = R"(
    <style>
      /* Force scrolling on all pages - fixes LinkedIn, ESPN, etc. */
      html, body {
        overflow: auto !important;
        -webkit-overflow-scrolling: touch !important;
        height: auto !important;
        position: static !important;
      }
      body {
        width: auto !important;
        min-height: 100vh !important;
      }
      /* Remove blocking iframes and overlays */
      iframe[src*="linkedin.com/app"],
      iframe[src*="install"],
      iframe[src*="download"],
      div[class*="modal-backdrop"],
      div[class*="overlay"],
      div[id*="overlay"] {
        display: none !important;
      }
      /* Ensure content is scrollable */
      #main, main, [role="main"], .content, #content {
        overflow: visible !important;
        height: auto !important;
      }
    </style>
  )";
  
  size_t head_end = html.find("</head>");
  if (head_end != std::string::npos) {
    html.insert(head_end, scroll_fix_css);
    LOG(INFO) << " Added scroll-fix CSS before </head>";
  } else {
    // If no </head>, insert after <html> tag
    size_t html_tag = html.find("<html");
    if (html_tag != std::string::npos) {
      size_t insert_pos = html.find('>', html_tag);
      if (insert_pos != std::string::npos) {
        html.insert(insert_pos + 1, "\n<head>" + scroll_fix_css + "</head>\n");
        LOG(INFO) << " Created <head> and added scroll-fix CSS";
      }
    }
  }
}

// static
void WootzMHTMLToHTMLConverter::AddViewportMetaTag(std::string& html) {
  // Check if viewport meta tag already exists
  if (html.find("name=\"viewport\"") != std::string::npos) {
    LOG(INFO) << " Viewport meta tag already exists, skipping";
    return;
  }
  
  std::string viewport_meta = 
      "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=yes\">\n";
  
  size_t head_start = html.find("<head>");
  if (head_start != std::string::npos) {
    html.insert(head_start + 6, "\n" + viewport_meta);
    LOG(INFO) << " Added viewport meta tag";
  } else {
    // Try to add after <html> if no <head>
    size_t html_tag = html.find("<html");
    if (html_tag != std::string::npos) {
      size_t insert_pos = html.find('>', html_tag);
      if (insert_pos != std::string::npos) {
        html.insert(insert_pos + 1, "\n<head>\n" + viewport_meta + "</head>\n");
        LOG(INFO) << " Created <head> and added viewport meta tag";
      }
    }
  }
}

}  // namespace wootz_offline_pages
