// Copyright 2024 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wootz_offline_pages/wootz_offline_page_service.h"

#include <set>

#include "base/android/jni_string.h"
#include "base/android/scoped_java_ref.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/hash/sha1.h"
#include "base/json/json_writer.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/wootz_offline_pages/wootz_mhtml_to_html_converter.h"
#include "chrome/common/chrome_paths.h"
#include "content/public/common/mhtml_generation_params.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/browser_thread.h"
#include "net/base/filename_util.h"

// Generated JNI headers
#include "chrome/android/chrome_jni_headers/WootzOfflinePagePathUtils_jni.h"

WootzOfflinePageService::WootzOfflinePageService() {
  // Ensure 404 page exists when service is created
  EnsureOffline404PageExists();
}

WootzOfflinePageService::~WootzOfflinePageService() = default;

void WootzOfflinePageService::SavePage(
    content::WebContents* web_contents,
    const std::vector<GURL>& redirect_chain) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  GURL url = web_contents->GetLastCommittedURL();
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS())
    return;

  LOG(INFO) << " WootzOfflinePageService::SavePage for " << url.spec();
  
  // Generate temporary MHTML path (will be converted to HTML)
  base::FilePath html_path = URLToFilePath(url);  // This returns .html path
  base::FilePath mhtml_temp_path = html_path.RemoveExtension().AddExtension(FILE_PATH_LITERAL("tmp"));
  
  // Verify storage directory exists and is writable
  base::FilePath storage_dir = html_path.DirName();
  if (!base::DirectoryExists(storage_dir)) {
    LOG(ERROR) << " Storage directory does not exist: " << storage_dir.value();
    return;
  }
  
  // Check if we have write permissions
  if (!base::PathIsWritable(storage_dir)) {
    LOG(ERROR) << " Storage directory is not writable: " << storage_dir.value();
    return;
  }
  
  LOG(INFO) << " Starting MHTML generation to: " << mhtml_temp_path.value();
  
  content::MHTMLGenerationParams params(mhtml_temp_path);
  // Set cache control to use cached resources (faster, more reliable)
  params.use_binary_encoding = true;  // More efficient encoding
  params.remove_popup_overlay = true;  // Remove popups for cleaner MHTML
  
  web_contents->GenerateMHTML(
      params, base::BindOnce(&WootzOfflinePageService::OnMHTMLGenerated,
                             weak_factory_.GetWeakPtr(), redirect_chain,
                             mhtml_temp_path));
}

void WootzOfflinePageService::OnMHTMLGenerated(
    const std::vector<GURL>& redirect_chain,
    const base::FilePath& file_path,
    int64_t size) {
  if (size <= 0) {
    LOG(ERROR) << " MHTML generation failed for "
               << redirect_chain.back().spec()
               << " - size: " << size 
               << " (this usually means the renderer process crashed or timed out)";
    
    // Check if file was partially created
    if (base::PathExists(file_path)) {
      auto actual_size = base::GetFileSize(file_path);
      if (actual_size.has_value()) {
        LOG(ERROR) << " Partial file exists with size: " << actual_size.value() << " bytes - deleting it";
      }
      base::DeleteFile(file_path);
    } else {
      LOG(ERROR) << " Output file was not created at all: " << file_path.value();
    }
    return;
  }
  
  LOG(INFO) << " MHTML generated successfully for " 
            << redirect_chain.back().spec()
            << " - file: " << file_path.value()
            << " - size: " << size << " bytes";
  
  // Convert MHTML to HTML for full JavaScript functionality
  // file_path is hash.tmp, so RemoveExtension gives hash, then add .html
  base::FilePath html_path = file_path.RemoveExtension().AddExtension(FILE_PATH_LITERAL("html"));
  
  LOG(INFO) << " Converting MHTML to HTML: " << file_path.value() 
            << " -> " << html_path.value();
  
  bool conversion_success = wootz_offline_pages::WootzMHTMLToHTMLConverter::Convert(
      file_path, html_path);
  
  if (conversion_success) {
    LOG(INFO) << " Successfully converted to HTML, deleting temporary MHTML file";
    
    // Delete the temporary MHTML file - we only keep the HTML
    if (!base::DeleteFile(file_path)) {
      LOG(WARNING) << " Failed to delete temporary MHTML file: " << file_path.value();
    }
    
    // Get the actual file size after conversion
    auto html_size_opt = base::GetFileSize(html_path);
    int64_t html_size = html_size_opt.value_or(0);
    
    LOG(INFO) << " Offline page saved as HTML: " << html_path.value()
              << " (size: " << html_size << " bytes)";
    
    // Update unified session manifest with this page and its redirect chain
    UpdateSessionManifest(html_path, redirect_chain, html_size);
  } else {
    LOG(ERROR) << " Failed to convert MHTML to HTML, keeping MHTML file as fallback";
    // Keep the MHTML file if conversion fails
  }
}

bool WootzOfflinePageService::GetOfflinePagePath(const GURL& url,
                                                 base::FilePath* path) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  // Allow blocking for file I/O operations
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                 base::BlockingType::MAY_BLOCK);
  
  *path = URLToFilePath(url);
  
  // First, check if the file exists at the direct path
  if (base::PathExists(*path)) {
    LOG(INFO) << " Found offline page at direct path: " << path->value();
    return true;
  }
  
  // Second, search session manifest for URL mappings (handles redirects efficiently)
  LOG(INFO) << " File not found at direct path, checking session manifest...";
  if (FindPagePathFromSessionManifest(url, path)) {
    LOG(INFO) << " Found page via session manifest: " << path->value();
    return true;
  }
  
  // If not found in the main directory, search subdirectories
  // This handles cases where users copy exported directories back
  LOG(INFO) << " Not found in manifests, searching subdirectories...";
  
  // Extract just the filename from the full path
  std::string filename = path->BaseName().AsUTF8Unsafe();
  
  JNIEnv* env = base::android::AttachCurrentThread();
  base::android::ScopedJavaLocalRef<jstring> j_filename =
      base::android::ConvertUTF8ToJavaString(env, filename);
  
  base::android::ScopedJavaLocalRef<jstring> j_found_path =
      wootz_offline_pages::Java_WootzOfflinePagePathUtils_findOfflinePageInSubdirectories(
          env, j_filename);
  
  if (!j_found_path.is_null()) {
    std::string found_path = base::android::ConvertJavaStringToUTF8(env, j_found_path);
    *path = base::FilePath(found_path);
    LOG(INFO) << " Found offline page in subdirectory: " << found_path;
    return true;
  }
  
  LOG(INFO) << " Offline page not found anywhere";
  return false;
}

bool WootzOfflinePageService::ClearAllPages() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  base::FilePath storage_dir = GetStorageDir();
  
  if (!base::DirectoryExists(storage_dir)) {
    LOG(INFO) << " Storage directory does not exist, nothing to clear";
    return true;
  }
  
  // Delete all contents of the storage directory
  bool success = base::DeletePathRecursively(storage_dir);
  
  if (success) {
    LOG(INFO) << " Successfully cleared all offline pages from " 
              << storage_dir.value();
    // Recreate the directory for future use
    base::CreateDirectory(storage_dir);
  } else {
    LOG(ERROR) << " Failed to clear offline pages from " 
               << storage_dir.value();
  }
  
  return success;
}

base::FilePath WootzOfflinePageService::GetStorageDir() {
  // Get external storage path from Java
  JNIEnv* env = base::android::AttachCurrentThread();
  base::android::ScopedJavaLocalRef<jstring> j_path =
      wootz_offline_pages::Java_WootzOfflinePagePathUtils_getOfflinePageStoragePath(env);
  
  if (j_path.is_null()) {
    LOG(ERROR) << " Failed to get external storage path from Java, "
               << "falling back to internal storage";
    // Fallback to internal storage if external is unavailable
    base::FilePath path;
    base::PathService::Get(chrome::DIR_USER_DATA, &path);
    path = path.Append(FILE_PATH_LITERAL("WootzOfflinePages"));
    if (!base::DirectoryExists(path))
      base::CreateDirectory(path);
    return path;
  }
  
  std::string path_str = base::android::ConvertJavaStringToUTF8(env, j_path);
  base::FilePath path(path_str);
  
  // Ensure directory exists
  if (!base::DirectoryExists(path)) {
    if (!base::CreateDirectory(path)) {
      LOG(ERROR) << " Failed to create external storage directory: " 
                 << path.value();
    }
  }
  
  LOG(INFO) << " Using external storage for offline pages: " << path.value();
  return path;
}

GURL WootzOfflinePageService::CanonicalizeUrlForOfflinePage(const GURL& url) {
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return url;
  }
  
  std::string path(url.path());
  
  // Step 1: Check if this is a search page (any site)
  // Search pages include: /search, /results, /find, etc.
  bool is_search_page = (path.find("/search") != std::string::npos ||
                         path.find("/results") != std::string::npos ||
                         path.find("/find") != std::string::npos);
  
  // Also check query parameters for common search indicators
  if (url.has_query() && !is_search_page) {
    std::string query(url.query());
    is_search_page = (query.find("q=") != std::string::npos ||
                     query.find("query=") != std::string::npos ||
                     query.find("search=") != std::string::npos ||
                     query.find("keyword=") != std::string::npos ||
                     query.find("keywords=") != std::string::npos); 
  }
  
  LOG(INFO) << " [URL CANONICALIZATION] " << url.spec() 
            << " -> is_search_page: " << (is_search_page ? "YES" : "NO");
  
  GURL::Replacements replacements;

  replacements.ClearQuery();
  replacements.ClearRef();
  
  GURL canonical = url.ReplaceComponents(replacements);
  
  // Step 2: Normalize trailing slash AFTER stripping query params
  // This ensures consistent hashing whether URL has query params or not
  // Example: /home and /home?trk=a both become /home (no slash)
  // Example: /jobs/ and /jobs/?trk=a both become /jobs/ (with slash)
  // std::string canonical_path = canonical.path();
  
  // Don't modify if path is empty, is root (/), or already ends with slash
  // Don't add slash if path looks like a file (has extension)
  // bool looks_like_file = (canonical_path.find_last_of('.') != std::string::npos &&
  //                         canonical_path.find_last_of('.') > canonical_path.find_last_of('/'));
  
  // For consistency: if path doesn't end with / and is not a file, DON'T add slash
  // This matches most saved URLs which typically don't have trailing slashes
  // Just return as-is after query/fragment stripping
  
  return canonical;
}

base::FilePath WootzOfflinePageService::URLToFilePath(const GURL& url) {
  // Use canonical URL for consistent file naming
  GURL canonical = CanonicalizeUrlForOfflinePage(url);
  
  std::string hash = base::SHA1HashString(canonical.spec());
  std::string hex_hash = base::HexEncode(hash.c_str(), hash.length());
  // Changed from .mhtml to .html since we now convert and save as HTML
  std::string filename = "wootz_offline_" + hex_hash + ".html";
  
  LOG(INFO) << " URL canonicalization: " << url.spec() 
            << " -> " << canonical.spec() << " -> " << filename;
  
  return GetStorageDir().Append(base::FilePath::FromUTF8Unsafe(filename));
}

base::FilePath WootzOfflinePageService::Get404PagePath() {
  JNIEnv* env = base::android::AttachCurrentThread();
  base::android::ScopedJavaLocalRef<jstring> j_path =
      wootz_offline_pages::Java_WootzOfflinePagePathUtils_get404PagePath(env);
  
  if (j_path.is_null()) {
    LOG(ERROR) << " Failed to get 404 page path from Java";
    return base::FilePath();
  }
  
  std::string path_str = base::android::ConvertJavaStringToUTF8(env, j_path);
  return base::FilePath(path_str);
}

bool WootzOfflinePageService::Does404PageExist() {
  JNIEnv* env = base::android::AttachCurrentThread();
  return wootz_offline_pages::Java_WootzOfflinePagePathUtils_does404PageExist(env);
}

void WootzOfflinePageService::EnsureOffline404PageExists() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  
  // Check if 404 page already exists
  if (Does404PageExist()) {
    LOG(INFO) << " 404 page already exists, skipping creation";
    return;
  }
  
  LOG(INFO) << " Creating offline 404 page";
  
  // Generate HTML content for the 404 page (no longer using MHTML)
  std::string html_content = 
      "<!DOCTYPE html>\n"
      "<html>\n"
      "<head>\n"
      "    <meta charset=\"utf-8\">\n"
      "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
      "    <title>Page Not Available Offline</title>\n"
      "    <style>\n"
      "        html, body {\n"
      "            height: 100vh;\n"
      "            width: 100vw;\n"
      "            margin: 0;\n"
      "            padding: 0;\n"
      "            overflow: hidden;\n"
      "        }\n"
      "        body {\n"
      "            font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, sans-serif;\n"
      "            display: flex;\n"
      "            align-items: center;\n"
      "            justify-content: center;\n"
      "            background: #f5f5f5;\n"
      "        }\n"
      "        .container {\n"
      "            text-align: center;\n"
      "            max-width: 450px;\n"
      "            background: white;\n"
      "            padding: 24px 20px;\n"
      "            border-radius: 12px;\n"
      "            box-shadow: 0 2px 10px rgba(0,0,0,0.1);\n"
      "            margin: 0 16px;\n"
      "        }\n"
      "        .icon { font-size: 64px; margin: 0 0 16px 0; }\n"
      "        h2 { font-size: 22px; margin: 0 0 12px 0; color: #333; }\n"
      "        p { color: #666; line-height: 1.5; margin: 0 0 16px 0; font-size: 14px; }\n"
      "        .info {\n"
      "            background: #e3f2fd;\n"
      "            padding: 12px;\n"
      "            border-radius: 8px;\n"
      "            margin: 16px 0;\n"
      "            color: #1976d2;\n"
      "            font-size: 13px;\n"
      "            line-height: 1.4;\n"
      "            text-align: left;\n"
      "        }\n"
      "    </style>\n"
      "</head>\n"
      "<body>\n"
      "    <div class=\"container\">\n"
      "        <div class=\"icon\">📭</div>\n"
      "        <h2>Page Not Available Offline</h2>\n"
      "        <p>This page hasn't been saved for offline viewing yet.</p>\n"
      "        <div class=\"info\">\n"
      "            <strong>Tip:</strong> To view this page offline, visit it while online "
      "with auto-save enabled. The page will be automatically saved for future "
      "offline access.\n"
      "        </div>\n"
      "        <p style=\"color: #999; font-size: 11px; margin-top: 16px; margin-bottom: 0;\">\n"
      "            Wootz Offline Pages\n"
      "        </p>\n"
      "    </div>\n"
      "</body>\n"
      "</html>\n";
  
  // Call Java method to create the file
  JNIEnv* env = base::android::AttachCurrentThread();
  base::android::ScopedJavaLocalRef<jstring> j_content =
      base::android::ConvertUTF8ToJavaString(env, html_content);
  
  bool success = wootz_offline_pages::Java_WootzOfflinePagePathUtils_create404Page(
      env, j_content);
  
  if (success) {
    LOG(INFO) << " Successfully created offline 404 page as HTML";
  } else {
    LOG(ERROR) << " Failed to create offline 404 page";
  }
}

base::FilePath WootzOfflinePageService::GetNoResultFoundPagePath() {
  JNIEnv* env = base::android::AttachCurrentThread();
  base::android::ScopedJavaLocalRef<jstring> j_path =
      wootz_offline_pages::Java_WootzOfflinePagePathUtils_getNoResultFoundPagePath(env);
  
  if (j_path.is_null()) {
    LOG(ERROR) << " Failed to get no result found page path from Java";
    return base::FilePath();
  }
  
  std::string path_str = base::android::ConvertJavaStringToUTF8(env, j_path);
  return base::FilePath(path_str);
}

bool WootzOfflinePageService::DoesNoResultFoundPageExist() {
  JNIEnv* env = base::android::AttachCurrentThread();
  return wootz_offline_pages::Java_WootzOfflinePagePathUtils_doesNoResultFoundPageExist(env);
}

// ============================================================================
// MULTI-MANIFEST SYSTEM
// Supports multiple coexisting manifest files from different devices/sessions
// ============================================================================

// Helper: Create empty manifest with timestamp
base::DictValue CreateEmptyManifest() {
  base::DictValue manifest;
  base::Time now = base::Time::Now();
  base::Time::Exploded exploded;
  now.UTCExplode(&exploded);
  std::string timestamp = base::StringPrintf(
      "%04d-%02d-%02dT%02d:%02d:%02dZ",
      exploded.year, exploded.month, exploded.day_of_month,
      exploded.hour, exploded.minute, exploded.second);
  
  manifest.Set("version", 1);
  manifest.Set("session_started", timestamp);
  manifest.Set("last_updated", timestamp);
  manifest.Set("page_count", 0);
  manifest.Set("url_mappings", base::DictValue());
  manifest.Set("pages", base::DictValue());
  
  return manifest;
}

// Load a single manifest file (returns empty dict if invalid, doesn't delete)
base::DictValue WootzOfflinePageService::LoadSingleManifest(
    const base::FilePath& manifest_path) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  
  if (!base::PathExists(manifest_path)) {
    return base::DictValue();
  }
  
  // Read manifest file
  std::string manifest_content;
  if (!base::ReadFileToString(manifest_path, &manifest_content)) {
    LOG(WARNING) << " [MANIFEST] Failed to read: " << manifest_path.BaseName().value()
                 << " (skipping, not deleting)";
    return base::DictValue();
  }
  
  // Parse JSON
  auto parsed = base::JSONReader::Read(manifest_content, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    LOG(WARNING) << " [MANIFEST] Invalid JSON in: " << manifest_path.BaseName().value()
                 << " (skipping, not deleting - may be manually editable)";
    return base::DictValue();
  }
  
  LOG(INFO) << " [MANIFEST] Successfully loaded: " << manifest_path.BaseName().value();
  return std::move(parsed->GetDict());
}

// Load ALL session manifests from storage directory
std::vector<base::DictValue> WootzOfflinePageService::LoadAllSessionManifests() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  
  std::vector<base::DictValue> all_manifests;
  base::FilePath storage_dir = GetStorageDir();
  
  if (!base::DirectoryExists(storage_dir)) {
    LOG(INFO) << " [MANIFEST] Storage directory doesn't exist yet";
    return all_manifests;
  }
  
  // Enumerate all session_manifest*.json files
  base::FileEnumerator enumerator(
      storage_dir,
      false,  // non-recursive
      base::FileEnumerator::FILES,
      FILE_PATH_LITERAL("session_manifest*.json"));
  
  int total_found = 0;
  int valid_count = 0;
  int invalid_count = 0;
  
  for (base::FilePath manifest_path = enumerator.Next(); 
       !manifest_path.empty(); 
       manifest_path = enumerator.Next()) {
    total_found++;
    
    base::DictValue manifest = LoadSingleManifest(manifest_path);
    if (!manifest.empty()) {
      all_manifests.push_back(std::move(manifest));
      valid_count++;
    } else {
      invalid_count++;
    }
  }
  
  LOG(INFO) << " [MANIFEST] Found " << total_found << " manifest files: "
            << valid_count << " valid, " << invalid_count << " invalid (skipped)";
  
  return all_manifests;
}

// Get path for a NEW timestamped manifest file
base::FilePath WootzOfflinePageService::GetNewSessionManifestPath() {
  base::FilePath storage_dir = GetStorageDir();
  
  base::Time now = base::Time::Now();
  base::Time::Exploded exploded;
  now.LocalExplode(&exploded);  // Use local time for filename readability
  
  std::string filename = base::StringPrintf(
      "session_manifest_%04d-%02d-%02d_%02d%02d%02d.json",
      exploded.year, exploded.month, exploded.day_of_month,
      exploded.hour, exploded.minute, exploded.second);
  
  return storage_dir.Append(base::FilePath::FromUTF8Unsafe(filename));
}

// Get current session's manifest path (cached, or create new timestamped one)
base::FilePath WootzOfflinePageService::GetCurrentSessionManifestPath() {
  // If we already have a manifest path for this session, reuse it
  if (!current_session_manifest_path_.empty() && 
      base::PathExists(current_session_manifest_path_)) {
    return current_session_manifest_path_;
  }
  
  // Create a new timestamped manifest path
  current_session_manifest_path_ = GetNewSessionManifestPath();
  LOG(INFO) << " [MANIFEST] Created new session manifest: " 
            << current_session_manifest_path_.BaseName().value();
  
  return current_session_manifest_path_;
}

// Save manifest to a specific path
bool WootzOfflinePageService::SaveSessionManifest(
    const base::DictValue& manifest,
    const base::FilePath& manifest_path) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  
  // Convert to JSON string with pretty printing
  std::string json_string;
  if (!base::JSONWriter::WriteWithOptions(
          manifest, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json_string)) {
    LOG(ERROR) << " [MANIFEST] Failed to serialize JSON";
    return false;
  }
  
  // Write to file
  if (base::WriteFile(manifest_path, json_string)) {
    LOG(INFO) << " [MANIFEST] Saved: " << manifest_path.BaseName().value()
              << " (" << json_string.length() << " bytes)";
    return true;
  } else {
    LOG(ERROR) << " [MANIFEST] Failed to write file: " 
               << manifest_path.value();
    return false;
  }
}

// Update the current session's manifest with a new page
void WootzOfflinePageService::UpdateSessionManifest(
    const base::FilePath& html_path,
    const std::vector<GURL>& redirect_chain,
    int64_t size) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  
  if (redirect_chain.empty()) {
    LOG(WARNING) << " [MANIFEST] Cannot update with empty redirect chain";
    return;
  }
  
  LOG(INFO) << " [MANIFEST UPDATE] Saving page: " << html_path.BaseName().value();
  LOG(INFO) << " [MANIFEST UPDATE] Redirect chain has " << redirect_chain.size() << " URLs:";
  for (size_t i = 0; i < redirect_chain.size(); i++) {
    LOG(INFO) << " [MANIFEST UPDATE]   [" << i << "] " << redirect_chain[i].spec();
  }
  
  // Get current session's manifest path (timestamped, cached for this session)
  base::FilePath manifest_path = GetCurrentSessionManifestPath();
  
  // Load existing manifest from this session (or create new one)
  base::DictValue manifest = LoadSingleManifest(manifest_path);
  if (manifest.empty()) {
    LOG(INFO) << " [MANIFEST] Creating new session manifest";
    manifest = CreateEmptyManifest();
  }
  
  // Update timestamp
  base::Time now = base::Time::Now();
  base::Time::Exploded exploded;
  now.UTCExplode(&exploded);
  std::string timestamp = base::StringPrintf(
      "%04d-%02d-%02dT%02d:%02d:%02dZ",
      exploded.year, exploded.month, exploded.day_of_month,
      exploded.hour, exploded.minute, exploded.second);
  manifest.Set("last_updated", timestamp);
  
  // Get filename
  std::string html_filename = html_path.BaseName().AsUTF8Unsafe();
  
  // Get or create url_mappings dict
  base::DictValue* url_mappings = manifest.FindDict("url_mappings");
  if (!url_mappings) {
    manifest.Set("url_mappings", base::DictValue());
    url_mappings = manifest.FindDict("url_mappings");
  }
  
  // Get or create pages dict
  base::DictValue* pages = manifest.FindDict("pages");
  if (!pages) {
    manifest.Set("pages", base::DictValue());
    pages = manifest.FindDict("pages");
  }
  
  // Add page entry with metadata
  base::DictValue page_info;
  page_info.Set("primary_url", redirect_chain.back().spec());
  page_info.Set("saved_at", timestamp);
  page_info.Set("size_bytes", static_cast<int>(size));
  
  // Canonical URL
  GURL canonical_primary = CanonicalizeUrlForOfflinePage(redirect_chain.back());
  page_info.Set("canonical_url", canonical_primary.spec());
  
  // Redirect chain
  base::ListValue redirect_list;
  for (const GURL& url : redirect_chain) {
    redirect_list.Append(url.spec());
  }
  page_info.Set("redirect_chain", std::move(redirect_list));
  
  pages->Set(html_filename, std::move(page_info));
  
  // TRIPLE MAPPING: Map all URLs in redirect chain to this HTML file
  // This handles: original URLs, canonical URLs, and subdomain variants
  std::set<std::string> processed_urls;
  int mapping_count = 0;
  
  for (const GURL& url : redirect_chain) {
    // 1. Add original URL with all query parameters
    std::string original_spec = url.spec();
    if (processed_urls.find(original_spec) == processed_urls.end()) {
      url_mappings->Set(original_spec, html_filename);
      processed_urls.insert(original_spec);
      mapping_count++;
      LOG(INFO) << " URL mapping: " << original_spec << " -> " << html_filename;
    }
    
    // 2. Add canonical URL (normalized - without query params for non-search pages)
    GURL canonical = CanonicalizeUrlForOfflinePage(url);
    std::string canonical_spec = canonical.spec();
    if (processed_urls.find(canonical_spec) == processed_urls.end()) {
      url_mappings->Set(canonical_spec, html_filename);
      processed_urls.insert(canonical_spec);
      mapping_count++;
      if (canonical_spec != original_spec) {
        LOG(INFO) << " URL mapping (canonical): " << canonical_spec << " -> " << html_filename;
      }
    }
    
    // 3. Add subdomain variants (www, in, m, mobile, etc.)
    std::vector<GURL> subdomain_variants = GenerateSubdomainVariants(canonical);
    for (const GURL& variant : subdomain_variants) {
      std::string variant_spec = variant.spec();
      if (processed_urls.find(variant_spec) == processed_urls.end()) {
        url_mappings->Set(variant_spec, html_filename);
        processed_urls.insert(variant_spec);
        mapping_count++;
        if (variant_spec != canonical_spec && variant_spec != original_spec) {
          LOG(INFO) << " URL mapping (subdomain variant): " << variant_spec << " -> " << html_filename;
        }
      }
    }
  }
  
  // Update page count
  manifest.Set("page_count", static_cast<int>(pages->size()));
  
  // Save updated manifest to current session's manifest file
  if (SaveSessionManifest(manifest, manifest_path)) {
    LOG(INFO) << " [MANIFEST] Updated with " << mapping_count 
              << " URL mappings for " << html_filename;
  }
}

// Find page path by searching ALL session manifests (supports multi-device)
bool WootzOfflinePageService::FindPagePathFromSessionManifest(const GURL& url,
                                                               base::FilePath* path) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  
  base::FilePath storage_dir = GetStorageDir();
  if (!base::DirectoryExists(storage_dir)) {
    return false;
  }
  
  // Load ALL session manifests (from all devices/sessions)
  std::vector<base::DictValue> all_manifests = LoadAllSessionManifests();
  
  if (all_manifests.empty()) {
    LOG(INFO) << " [MANIFEST LOOKUP] No valid manifests found";
    return false;
  }
  
  // Prepare URLs for lookup
  std::string original_url_spec = url.spec();
  GURL canonical_url = CanonicalizeUrlForOfflinePage(url);
  std::string canonical_url_spec = canonical_url.spec();
  
  LOG(INFO) << " [MANIFEST LOOKUP] Searching " << all_manifests.size() 
            << " manifest(s) for: " << original_url_spec;
  if (original_url_spec != canonical_url_spec) {
    LOG(INFO) << " [MANIFEST LOOKUP] Also trying canonical: " << canonical_url_spec;
  }
  
  // Prepare subdomain variants for comprehensive search
  std::vector<GURL> subdomain_variants = GenerateSubdomainVariants(canonical_url);
  std::vector<std::string> all_url_variants;
  all_url_variants.push_back(original_url_spec);
  if (original_url_spec != canonical_url_spec) {
    all_url_variants.push_back(canonical_url_spec);
  }
  for (const GURL& variant : subdomain_variants) {
    std::string variant_spec = variant.spec();
    if (variant_spec != original_url_spec && variant_spec != canonical_url_spec) {
      all_url_variants.push_back(variant_spec);
    }
  }
  
  // Search through all manifests
  int manifest_num = 0;
  for (const auto& manifest : all_manifests) {
    manifest_num++;
    const base::DictValue* url_mappings = manifest.FindDict("url_mappings");
    
    if (!url_mappings || url_mappings->empty()) {
      continue;  // Skip empty manifest
    }
    
    // Try all URL variants
    for (const std::string& url_variant : all_url_variants) {
      const std::string* html_filename = url_mappings->FindString(url_variant);
      
      if (html_filename && !html_filename->empty()) {
        // Found it! Check if file exists
        *path = storage_dir.Append(base::FilePath::FromUTF8Unsafe(*html_filename));
        
        if (base::PathExists(*path)) {
          std::string match_desc = (url_variant == original_url_spec) ? "original" :
                                   (url_variant == canonical_url_spec) ? "canonical" : "subdomain";
          LOG(INFO) << " [MANIFEST LOOKUP] ✓ Found in manifest #" << manifest_num 
                    << " (" << match_desc << " match): " << *html_filename;
          return true;
        } else {
          LOG(WARNING) << " [MANIFEST LOOKUP] Manifest #" << manifest_num 
                       << " points to non-existent file: " << *html_filename;
        }
      }
    }
  }
  
  LOG(INFO) << " [MANIFEST LOOKUP] ✗ URL not found in any of " 
            << all_manifests.size() << " manifest(s)";
  return false;
}

std::vector<GURL> WootzOfflinePageService::GenerateSubdomainVariants(const GURL& url) {
  std::vector<GURL> variants;
  
  if (!url.is_valid() || !url.has_host()) {
    variants.push_back(url);
    return variants;
  }
  
  std::string host(url.host());
  
  // Common subdomain prefixes to generate variants for
  std::vector<std::string> common_subdomains = {"www", "m", "mobile", "in"};
  
  // Always add the original URL first
  variants.push_back(url);
  
  // Extract base domain (e.g., linkedin.com from www.linkedin.com)
  size_t first_dot = host.find('.');
  if (first_dot == std::string::npos) {
    // No subdomain, just domain.tld - return as-is
    return variants;
  }
  
  // Check if this already has a subdomain
  std::string potential_subdomain = host.substr(0, first_dot);
  std::string base_domain = host.substr(first_dot + 1);
  
  // If base domain doesn't have a dot, this is likely domain.tld (no subdomain)
  if (base_domain.find('.') == std::string::npos) {
    return variants;
  }
  
  // Generate variants by swapping subdomains
  for (const std::string& subdomain : common_subdomains) {
    if (subdomain == potential_subdomain) {
      continue; // Skip if it's the same as current
    }
    
    std::string variant_host = subdomain + "." + base_domain;
    GURL::Replacements replacements;
    replacements.SetHostStr(variant_host);
    GURL variant_url = url.ReplaceComponents(replacements);
    
    if (variant_url.is_valid()) {
      variants.push_back(variant_url);
    }
  }
  
  // Also add base domain without subdomain (e.g., linkedin.com)
  GURL::Replacements replacements;
  replacements.SetHostStr(base_domain);
  GURL base_url = url.ReplaceComponents(replacements);
  if (base_url.is_valid()) {
    variants.push_back(base_url);
  }
  
  return variants;
}
