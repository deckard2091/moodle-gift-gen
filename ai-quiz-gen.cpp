#include <algorithm>
#include <chrono>
#include <random>
#include <cstring>
#include <cstdio>
#include "miniz.h"
#include <curl/curl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>
#include "gift-validator/gift-validator.hpp"

using json = nlohmann::json;

const std::string GEMINI_MODEL_FLASH = "gemini-2.5-flash";
const std::string GEMINI_MODEL_PRO = "gemini-2.5-pro";

size_t write_callback(void *contents, size_t size, size_t nmemb,
                      std::string *result)
{
  size_t total_size = size * nmemb;
  result->append((char *)contents, total_size);
  return total_size;
}

json generate_quiz_schema()
{
  json schema = {
      {"type", "object"},
      {"properties",
       {{"category",
         {{"type", "string"},
          {"description",
           "A short category name (less than 30 characters) based on the "
           "context of the provided files and prompt. This should summarize "
           "the topic or subject area of the questions."}}},
        {"questions",
         {{"type", "array"},
          {"items",
           {{"type", "object"},
            {"properties",
             {{"title",
               {{"type", "string"},
                {"description", "A short title for the question"}}},
              {"question",
               {{"type", "string"}, {"description", "The question text"}}},
              {"options",
               {{"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Array of answer options"}}},
              {"correct_answer",
               {{"type", "integer"},
                {"description", "Index of the correct answer (0-based)"}}},
              {"explanation",
               {{"type", "string"},
                {"description",
                 "Optional explanation for the correct answer"}}}}},
            {"required", json::array({"title", "question", "options",
                                      "correct_answer"})}}}}}}},
      {"required", json::array({"category", "questions"})}};
  return schema;
}

std::string query_gemini(const std::vector<std::string> &file_ids,
                         const std::string &query, const json &schema,
                         const std::string &api_key,
                         const std::string &model = GEMINI_MODEL_FLASH)
{
  CURL *curl;
  CURLcode res;
  std::string result;

  curl = curl_easy_init();
  if (!curl)
  {
    throw std::runtime_error("Failed to initialize CURL");
  }

  std::string url = "https://generativelanguage.googleapis.com/v1beta/models/" +
                    model + ":generateContent?key=" + api_key;

  json request_body = {{"contents", json::array()},
                       {"generationConfig",
                        {{"response_mime_type", "application/json"},
                         {"response_schema", schema}}}};

  json content = {{"parts", json::array()}};

  for (const auto &file_id : file_ids)
  {
    content["parts"].push_back(
        {{"file_data",
          {{"file_uri",
            "https://generativelanguage.googleapis.com/v1beta/files/" +
                file_id}}}});
  }

  content["parts"].push_back({{"text", query}});
  request_body["contents"].push_back(content);

  std::string json_data = request_body.dump();

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_data.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  res = curl_easy_perform(curl);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK)
  {
    throw std::runtime_error("CURL request failed: " +
                             std::string(curl_easy_strerror(res)));
  }

  return result;
}

// Fix unbalanced backticks caused by the LLM omitting the opening backtick of
// the first code span or the closing backtick of the last code span.
// Step 1: if the text starts with a code token (no space before the first
// backtick), it is missing its opening backtick, so prepend one.
// Step 2: if the backtick count is odd after step 1, the last code span is
// missing its closing backtick, so append one.
std::string fix_backticks(const std::string &text)
{
  std::string result = text;

  size_t first = result.find('`');
  if (first != std::string::npos && first > 0 && result.find(' ') > first)
    result = '`' + result;

  if (std::count(result.begin(), result.end(), '`') % 2 != 0)
    result += '`';

  return result;
}

// Replace actual newline characters in a Gemini-supplied field with the
// two-character literal \n sequence that Moodle accepts as a line break,
// preventing a blank line from splitting the question block in two.
std::string escape_newlines(const std::string &text)
{
  std::string result;
  result.reserve(text.size());
  for (const char c : text)
  {
    if (c == '\n')
      result += "\\n";
    else
      result += c;
  }
  return result;
}

std::string escape_gift_text(const std::string &text)
{
  std::string result;
  result.reserve(text.length() * 1.2); // Reserve extra space for escaping

  for (const char c : text)
  {
    // Escape GIFT control characters
    if (c == '{' || c == '}' || c == '#' || c == ':' || c == '~' || c == '=')
    {
      result += '\\';
    }
    result += c;
  }

  return result;
}

std::string get_mime_type(const std::string &filename)
{
  // Find the last dot to get the file extension
  size_t dot_pos = filename.find_last_of('.');
  if (dot_pos == std::string::npos)
  {
    return "application/octet-stream"; // Default for unknown types
  }

  std::string extension = filename.substr(dot_pos + 1);

  // Convert extension to lowercase for case-insensitive comparison
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  // Map common file extensions to MIME types
  if (extension == "pdf")
    return "application/pdf";
  else if (extension == "txt")
    return "text/plain";
  else if (extension == "md" || extension == "markdown")
    return "text/markdown";
  else if (extension == "docx")
    return "application/"
           "vnd.openxmlformats-officedocument.wordprocessingml.document";
  else if (extension == "doc")
    return "application/msword";
  else if (extension == "pptx")
    return "application/"
           "vnd.openxmlformats-officedocument.presentationml.presentation";
  else if (extension == "ppt")
    return "application/vnd.ms-powerpoint";
  else if (extension == "xlsx")
    return "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
  else if (extension == "xls")
    return "application/vnd.ms-excel";
  else if (extension == "jpg" || extension == "jpeg")
    return "image/jpeg";
  else if (extension == "png")
    return "image/png";
  else if (extension == "gif")
    return "image/gif";
  else if (extension == "bmp")
    return "image/bmp";
  else if (extension == "webp")
    return "image/webp";
  else if (extension == "tiff" || extension == "tif")
    return "image/tiff";
  else if (extension == "svg")
    return "image/svg+xml";
  else if (extension == "html" || extension == "htm")
    return "text/html";
  else if (extension == "json")
    return "application/json";
  else if (extension == "xml")
    return "application/xml";
  else if (extension == "csv")
    return "text/csv";
  else if (extension == "rtf")
    return "application/rtf";
  else
    return "application/octet-stream"; // Default for unknown types
}

std::string get_timestamp_suffix()
{
  auto now = std::chrono::system_clock::now();
  auto time_t_now = std::chrono::system_clock::to_time_t(now);
  std::tm local_tm = *std::localtime(&time_t_now);

  std::ostringstream oss;
  oss << std::put_time(&local_tm, "%A %d-%m-%Y %H:%M:%S");
  return oss.str();
}

std::string escape_xml_text(const std::string &text)
{
  std::string result;
  result.reserve(text.length() * 1.2);
  for (char c : text)
  {
    switch (c)
    {
      case '<': result += "&lt;"; break;
      case '>': result += "&gt;"; break;
      case '&': result += "&amp;"; break;
      case '"': result += "&quot;"; break;
      case '\'': result += "&apos;"; break;
      default: result += c;
    }
  }
  return result;
}

// Generate a v4-style GUID prefixed with 'i' (matches D2L's Common Cartridge idents).
static std::string make_guid()
{
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<int> hexdig(0, 15);
  const char *h = "0123456789abcdef";
  std::string u = "i";
  auto add = [&](int n) { for (int k = 0; k < n; ++k) u += h[hexdig(rng)]; };
  add(8); u += '-';
  add(4); u += "-4";        // version 4
  add(3); u += '-';
  u += h[(hexdig(rng) & 0x3) | 0x8]; // variant (8,9,a,b)
  add(3); u += '-';
  add(12);
  return u;
}

// Wrap plain text as a <p>...</p> HTML fragment, XML-escaped for use inside
// <mattext texttype="text/html">. This matches what Brightspace itself emits.
static std::string mattext_html(const std::string &text)
{
  return escape_xml_text("<p>" + text + "</p>");
}

void generate_qti_files(const json &quiz_data, const std::string &output_file,
                        const std::string &context_override, bool quiet)
{
  const std::string title = context_override.empty()
      ? (quiz_data.contains("category")
             ? quiz_data["category"].get<std::string>()
             : "Quiz")
      : context_override;

  const std::string assess_id = make_guid();
  const std::string section_id = make_guid();

  // ---- QTI 1.2 assessment (Common Cartridge profile) ----
  std::stringstream qti;
  qti << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
  qti << "<questestinterop xmlns=\"http://www.imsglobal.org/xsd/ims_qtiasiv1p2\" "
         "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
         "xsi:schemaLocation=\"http://www.imsglobal.org/xsd/ims_qtiasiv1p2 "
         "http://www.imsglobal.org/profile/cc/ccv1p3/ccv1p3_qtiasiv1p2p1_v1p0.xsd\">\n";
  qti << "  <assessment ident=\"" << assess_id << "\" title=\""
      << escape_xml_text(title) << "\">\n";
  qti << "    <qtimetadata>\n";
  qti << "      <qtimetadatafield><fieldlabel>cc_profile</fieldlabel><fieldentry>cc.exam.v0p1</fieldentry></qtimetadatafield>\n";
  qti << "      <qtimetadatafield><fieldlabel>qmd_assessmenttype</fieldlabel><fieldentry>Examination</fieldentry></qtimetadatafield>\n";
  qti << "      <qtimetadatafield><fieldlabel>cc_maxattempts</fieldlabel><fieldentry>1</fieldentry></qtimetadatafield>\n";
  qti << "    </qtimetadata>\n";
  qti << "    <section ident=\"" << section_id << "\">\n";

  int q_idx = 1;
  for (const auto &question : quiz_data["questions"])
  {
    // The GIFT path is filtered by gift::filter_valid; the CC path needs its own
    // guard so a malformed question is skipped rather than emitting broken scoring.
    if (!question.contains("question") || !question.contains("options") ||
        !question.contains("correct_answer") || !question["options"].is_array())
    {
      if (!quiet) std::cerr << "Skipping malformed question " << q_idx << std::endl;
      q_idx++; continue;
    }
    const auto &options = question["options"];
    const int correct_index = question["correct_answer"].get<int>();
    if (correct_index < 0 ||
        static_cast<size_t>(correct_index) >= options.size())
    {
      if (!quiet)
        std::cerr << "Skipping question " << q_idx
                  << " (correct_answer out of range)" << std::endl;
      q_idx++; continue;
    }

    const std::string item_id = make_guid();
    const std::string lid = make_guid();
    std::vector<std::string> opt_ids;
    opt_ids.reserve(options.size());
    for (size_t i = 0; i < options.size(); ++i) opt_ids.push_back(make_guid());

    qti << "      <item ident=\"" << item_id << "\">\n";
    qti << "        <itemmetadata>\n";
    qti << "          <qtimetadata>\n";
    qti << "            <qtimetadatafield><fieldlabel>cc_profile</fieldlabel><fieldentry>cc.multiple_choice.v0p1</fieldentry></qtimetadatafield>\n";
    qti << "            <qtimetadatafield><fieldlabel>cc_weighting</fieldlabel><fieldentry>1</fieldentry></qtimetadatafield>\n";
    qti << "          </qtimetadata>\n";
    qti << "        </itemmetadata>\n";
    qti << "        <presentation>\n";
    qti << "          <material><mattext texttype=\"text/html\">"
        << mattext_html(question["question"].get<std::string>())
        << "</mattext></material>\n";
    qti << "          <response_lid ident=\"" << lid << "\" rcardinality=\"Single\">\n";
    qti << "            <render_choice>\n";
    for (size_t i = 0; i < options.size(); ++i)
    {
      qti << "              <response_label ident=\"" << opt_ids[i] << "\">\n";
      qti << "                <material><mattext texttype=\"text/html\">"
          << mattext_html(options[i].get<std::string>())
          << "</mattext></material>\n";
      qti << "              </response_label>\n";
    }
    qti << "            </render_choice>\n";
    qti << "          </response_lid>\n";
    qti << "        </presentation>\n";
    qti << "        <resprocessing>\n";
    qti << "          <outcomes><decvar minvalue=\"0\" maxvalue=\"100\" varname=\"SCORE\" vartype=\"Decimal\"/></outcomes>\n";
    for (size_t i = 0; i < options.size(); ++i)
    {
      if (static_cast<int>(i) == correct_index)
      {
        qti << "          <respcondition continue=\"No\">\n";
        qti << "            <conditionvar><varequal respident=\"" << lid << "\">"
            << opt_ids[i] << "</varequal></conditionvar>\n";
        qti << "            <setvar action=\"Set\" varname=\"SCORE\">100</setvar>\n";
        qti << "          </respcondition>\n";
      }
      else
      {
        qti << "          <respcondition>\n";
        qti << "            <conditionvar><varequal respident=\"" << lid << "\">"
            << opt_ids[i] << "</varequal></conditionvar>\n";
        qti << "          </respcondition>\n";
      }
    }
    qti << "        </resprocessing>\n";
    qti << "      </item>\n";
    q_idx++;
  }

  qti << "    </section>\n";
  qti << "  </assessment>\n";
  qti << "</questestinterop>\n";

  // ---- Common Cartridge 1.3 manifest ----
  const std::string qti_href =
      "quiz/" + make_guid() + "/qti_" + make_guid() + ".xml";

  std::stringstream manifest;
  manifest << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
  manifest << "<manifest identifier=\"" << make_guid() << "\" "
              "xmlns=\"http://www.imsglobal.org/xsd/imsccv1p3/imscp_v1p1\" "
              "xmlns:lomr=\"http://ltsc.ieee.org/xsd/imsccv1p3/LOM/resource\" "
              "xmlns:lomm=\"http://ltsc.ieee.org/xsd/imsccv1p3/LOM/manifest\" "
              "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
              "xsi:schemaLocation=\""
              "http://ltsc.ieee.org/xsd/imsccv1p3/LOM/resource http://www.imsglobal.org/profile/cc/ccv1p3/LOM/ccv1p3_lomresource_v1p0.xsd "
              "http://www.imsglobal.org/xsd/imsccv1p3/imscp_v1p1 http://www.imsglobal.org/profile/cc/ccv1p3/ccv1p3_imscp_v1p2_v1p0.xsd "
              "http://ltsc.ieee.org/xsd/imsccv1p3/LOM/manifest http://www.imsglobal.org/profile/cc/ccv1p3/LOM/ccv1p3_lommanifest_v1p0.xsd\">\n";
  manifest << "  <metadata>\n";
  manifest << "    <schema>IMS Common Cartridge</schema>\n";
  manifest << "    <schemaversion>1.3.0</schemaversion>\n";
  manifest << "    <lomm:lom><lomm:general><lomm:title><lomm:string language=\"en-US\">"
           << escape_xml_text(title) << "</lomm:string></lomm:title></lomm:general></lomm:lom>\n";
  manifest << "  </metadata>\n";
  manifest << "  <organizations>\n";
  manifest << "    <organization identifier=\"" << make_guid() << "\" structure=\"rooted-hierarchy\">\n";
  manifest << "      <item identifier=\"" << make_guid() << "\"/>\n";
  manifest << "      <metadata><lomm:lom/></metadata>\n";
  manifest << "    </organization>\n";
  manifest << "  </organizations>\n";
  manifest << "  <resources>\n";
  manifest << "    <resource identifier=\"" << make_guid() << "_R\" type=\"imsqti_xmlv1p2/imscc_xmlv1p3/assessment\">\n";
  manifest << "      <file href=\"" << qti_href << "\"/>\n";
  manifest << "    </resource>\n";
  manifest << "  </resources>\n";
  manifest << "</manifest>\n";

  // ---- write the .zip with miniz, adding both entries from memory ----
  const std::string zip_path = output_file.empty() ? "quiz_cc.zip" : output_file;
  std::remove(zip_path.c_str());

  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof(zip));
  if (!mz_zip_writer_init_file(&zip, zip_path.c_str(), 0))
    throw std::runtime_error("Failed to create zip archive: " + zip_path);

  const std::string manifest_str = manifest.str();
  const std::string qti_str = qti.str();

  const bool ok =
      mz_zip_writer_add_mem(&zip, "imsmanifest.xml", manifest_str.data(),
                            manifest_str.size(), MZ_BEST_COMPRESSION) &&
      mz_zip_writer_add_mem(&zip, qti_href.c_str(), qti_str.data(),
                            qti_str.size(), MZ_BEST_COMPRESSION);

  if (!ok || !mz_zip_writer_finalize_archive(&zip))
  {
    mz_zip_writer_end(&zip);
    throw std::runtime_error("Failed to write Common Cartridge zip: " + zip_path);
  }
  mz_zip_writer_end(&zip);

  if (!quiet)
    std::cout << "Common Cartridge quiz saved to: " << zip_path << std::endl;
}

std::string convert_to_gift_format(const json &quiz_data,
                                   const std::string &context_override)
{
  std::stringstream gift_output;

  // Add category line at the top
  std::string category;
  if (!context_override.empty())
  {
    category = context_override;
  }
  else
  {
    if (quiz_data.contains("category"))
    {
      category = quiz_data["category"].get<std::string>();
    }
    else
    {
      category = "Quiz";
    }
    // Append timestamp to category only when LLM-generated
    category += " " + get_timestamp_suffix();
  }

  gift_output << "\n$CATEGORY: " << category << "\n\n";

  for (const auto &question : quiz_data["questions"])
  {
    // Guard the index before emitting anything. An out-of-range correct_answer
    // would otherwise produce a multichoice with no '=' answer; gift::filter_valid
    // only checks the answer count, not the presence of a correct answer, so such
    // a question would pass validation here yet be rejected by Moodle on import.
    const auto &options = question["options"];
    const int correct_index = question["correct_answer"].get<int>();
    if (correct_index < 0 ||
        static_cast<size_t>(correct_index) >= options.size())
    {
      std::cerr << "Skipping question (correct_answer out of range)" << std::endl;
      continue;
    }

    if (question.contains("title"))
    {
      gift_output << "::"
                  << escape_gift_text(escape_newlines(question["title"].get<std::string>()))
                  << "::\n";
    }
    gift_output << "[markdown]"
                << escape_gift_text(escape_newlines(fix_backticks(question["question"].get<std::string>())))
                << " {\n";

    for (size_t i = 0; i < options.size(); ++i)
    {
      const char c = (i == static_cast<size_t>(correct_index)) ? '=' : '~';
      gift_output << c << escape_gift_text(escape_newlines(fix_backticks(options[i].get<std::string>())))
                  << '\n';
    }

    gift_output << "}\n\n";
  }

  return gift_output.str();
}

struct UploadHandle
{
  CURL *curl;
  curl_mime *mime;
  std::string result;
  std::string filename;
  std::string file_id;
  bool completed;
  long response_code;
};

std::vector<std::string> upload_files(const std::vector<std::string> &filenames,
                                      const std::string &api_key,
                                      const bool quiet = false)
{
  if (filenames.empty())
    return {};

  if (!quiet)
    std::cout << "Starting parallel upload of " << filenames.size()
              << " files to Gemini..." << std::endl;

  CURLM *multi_handle = curl_multi_init();
  if (!multi_handle)
  {
    throw std::runtime_error("Failed to initialize CURL multi handle");
  }

  std::vector<UploadHandle> handles(filenames.size());
  std::string url =
      "https://generativelanguage.googleapis.com/upload/v1beta/files?key=" +
      api_key;

  // Setup all handles
  for (size_t i = 0; i < filenames.size(); ++i)
  {
    // Check if file exists before attempting upload
    std::ifstream file_check(filenames[i]);
    if (!file_check.good())
    {
      throw std::runtime_error("File not found: " + filenames[i]);
    }
    file_check.close();

    handles[i].curl = curl_easy_init();
    if (!handles[i].curl)
    {
      throw std::runtime_error("Failed to initialize CURL handle for " +
                               filenames[i]);
    }

    handles[i].filename = filenames[i];
    handles[i].completed = false;
    handles[i].response_code = 0;

    // Setup MIME data
    handles[i].mime = curl_mime_init(handles[i].curl);

    // Add metadata part
    json metadata = {
        {"file",
         {{"display_name",
           filenames[i].substr(filenames[i].find_last_of("/\\") + 1)}}}};

    curl_mimepart *part = curl_mime_addpart(handles[i].mime);
    curl_mime_name(part, "metadata");
    curl_mime_data(part, metadata.dump().c_str(), CURL_ZERO_TERMINATED);
    curl_mime_type(part, "application/json; charset=utf-8");

    // Add file part
    part = curl_mime_addpart(handles[i].mime);
    curl_mime_name(part, "file");
    curl_mime_filedata(part, filenames[i].c_str());
    curl_mime_type(part, get_mime_type(filenames[i]).c_str());

    // Configure curl options
    curl_easy_setopt(handles[i].curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handles[i].curl, CURLOPT_MIMEPOST, handles[i].mime);
    curl_easy_setopt(handles[i].curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handles[i].curl, CURLOPT_WRITEDATA, &handles[i].result);

    // Add to multi handle
    curl_multi_add_handle(multi_handle, handles[i].curl);
  }

  // Perform all transfers
  int running_handles;
  CURLMcode mc = curl_multi_perform(multi_handle, &running_handles);
  if (mc != CURLM_OK)
  {
    throw std::runtime_error("curl_multi_perform failed: " +
                             std::string(curl_multi_strerror(mc)));
  }

  // Wait for all transfers to complete
  while (running_handles > 0)
  {
    fd_set fdread{}, fdwrite{}, fdexcep{};
    int maxfd = -1;
    long curl_timeo = -1;

    curl_multi_timeout(multi_handle, &curl_timeo);
    if (curl_timeo < 0)
      curl_timeo = 1000;

    mc = curl_multi_fdset(multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);
    if (mc != CURLM_OK)
    {
      throw std::runtime_error("curl_multi_fdset failed: " +
                               std::string(curl_multi_strerror(mc)));
    }

    struct timeval timeout;
    timeout.tv_sec = curl_timeo / 1000;
    timeout.tv_usec = (curl_timeo % 1000) * 1000;

    int rc = select(maxfd + 1, &fdread, &fdwrite, &fdexcep, &timeout);
    if (rc < 0)
    {
      throw std::runtime_error("select() failed");
    }

    curl_multi_perform(multi_handle, &running_handles);
  }

  // Check results and extract file IDs
  std::vector<std::string> file_ids;
  file_ids.reserve(filenames.size());

  for (size_t i = 0; i < handles.size(); ++i)
  {
    curl_easy_getinfo(handles[i].curl, CURLINFO_RESPONSE_CODE,
                      &handles[i].response_code);

    if (handles[i].response_code != 200)
    {
      std::string error_msg = "Upload failed for " + handles[i].filename +
                              " with HTTP " +
                              std::to_string(handles[i].response_code);

      // Cleanup before throwing
      for (auto &handle : handles)
      {
        curl_multi_remove_handle(multi_handle, handle.curl);
        curl_mime_free(handle.mime);
        curl_easy_cleanup(handle.curl);
      }
      curl_multi_cleanup(multi_handle);

      throw std::runtime_error(error_msg);
    }

    // Parse response to get file ID
    if (!handles[i].result.empty())
    {
      json response = json::parse(handles[i].result);
      if (response.contains("file") && response["file"].contains("name"))
      {
        std::string file_name = response["file"]["name"];
        file_ids.push_back(file_name.substr(file_name.find_last_of('/') + 1));
      }
      else
      {
        // Cleanup before throwing
        for (auto &handle : handles)
        {
          curl_multi_remove_handle(multi_handle, handle.curl);
          curl_mime_free(handle.mime);
          curl_easy_cleanup(handle.curl);
        }
        curl_multi_cleanup(multi_handle);

        throw std::runtime_error(
            "Failed to parse file ID from upload response for " +
            handles[i].filename);
      }
    }
    else
    {
      // Cleanup before throwing
      for (auto &handle : handles)
      {
        curl_multi_remove_handle(multi_handle, handle.curl);
        curl_mime_free(handle.mime);
        curl_easy_cleanup(handle.curl);
      }
      curl_multi_cleanup(multi_handle);

      throw std::runtime_error("Empty response for " + handles[i].filename);
    }
  }

  // Cleanup
  for (auto &handle : handles)
  {
    curl_multi_remove_handle(multi_handle, handle.curl);
    curl_mime_free(handle.mime);
    curl_easy_cleanup(handle.curl);
  }
  curl_multi_cleanup(multi_handle);

  if (!quiet)
    std::cout << "Successfully uploaded all " << filenames.size()
              << " files to Gemini in parallel." << std::endl;

  return file_ids;
}

void run_quiz_generation(const int num_questions,
                         const std::vector<std::string> &file_ids,
                         const std::string &api_key,
                         const std::string &output_file = "",
                         const bool interactive = false,
                         const bool quiet = false,
                         const std::string &custom_prompt = "",
                         const std::string &context_override = "",
                         const std::string &output_format = "gift")
{
  json schema = generate_quiz_schema();
  std::string query;
  const std::string constraints =
            " Ensure these are formatted according to the provided"
            " json schema. Ensure that any code excerpts in the generated"
            " questions or answers are surrounded by a pair of backticks."
            " Also ensure each question includes a short title: if a question"
            " is based on content from a provided file, start the question"
            " title using a short version of the relevant file's title or"
            " overall theme. Do not refer to the files provided by an ordinal"
            " word, such as \"first\" or \"second\". When referring to an"
            " image, do this only using one or two words which relate to the"
            " content of the image itself; though vary (avoid) this if it"
            " might help answer the question. Also generate a short category"
            " name (less than 30 characters) that summarizes the topic or"
            " subject area of the questions based on the provided context.";

  if (!custom_prompt.empty())
  {
    query = custom_prompt + constraints;
  }
  else
  {
    query = "From both the text and images in the provided files, generate " +
            std::to_string(num_questions) +
            " multiple choice questions." + constraints;
  }

  bool satisfied = false;
  while (!satisfied)
  {
    std::string response = query_gemini(file_ids, query, schema, api_key);
    // std::cout << "Gemini response: " << response << std::endl;

    json response_json = json::parse(response);

    // Check for error responses
    if (response_json.contains("error"))
    {
      auto &error = response_json["error"];
      std::string error_msg = "Gemini API Error";

      if (error.contains("code"))
      {
        error_msg += " " + std::to_string(error["code"].get<int>());
      }

      if (error.contains("message"))
      {
        error_msg += ": " + error["message"].get<std::string>();
      }

      if (error.contains("status"))
      {
        error_msg += " (Status: " + error["status"].get<std::string>() + ")";
      }

      std::cerr << error_msg << std::endl;
      std::cout << "Try again? (y/n): ";
      std::string user_input;
      std::getline(std::cin, user_input);

      if (user_input != "y" && user_input != "Y" && user_input != "yes" &&
          user_input != "Yes")
      {
        throw std::runtime_error("User chose to exit after API error");
      }
      continue; // Skip to next iteration of the while loop
    }

    // Handle different response formats
    json quiz_data;
    if (response_json.contains("candidates") &&
        !response_json["candidates"].empty())
    {
      auto &candidate = response_json["candidates"][0];
      if (candidate.contains("content") &&
          candidate["content"].contains("parts") &&
          !candidate["content"]["parts"].empty())
      {
        auto &part = candidate["content"]["parts"][0];
        if (part.contains("text"))
        {
          quiz_data = json::parse(part["text"].get<std::string>());
        }
        else
        {
          quiz_data = part;
        }
      }
    }
    else
    {
      quiz_data = response_json;
    }

    std::string gift_output;
    if (output_format != "qti" || interactive)
    {
      std::string tmp = convert_to_gift_format(quiz_data, context_override);
      gift_output = gift::filter_valid(tmp);
    }

    if (interactive)
    {
      std::cout << "\n" << gift_output << std::endl;

      std::cout << "Is this output good enough? (y/n): ";
      std::string user_input;
      std::getline(std::cin, user_input);

      satisfied = (user_input == "y" || user_input == "Y" ||
                   user_input == "yes" || user_input == "Yes");
    }
    else
    {
      satisfied = true; // Exit after first generation
    }

    if (satisfied)
    {
      if (output_format == "qti")
      {
        generate_qti_files(quiz_data, output_file, context_override, quiet);
      }
      else
      {
        if (!output_file.empty())
        {
          std::ofstream file(output_file);
          if (!file.is_open())
          {
            throw std::runtime_error("Unable to open output file: " +
                                     output_file);
          }
          file << gift_output;
          file.close();
          if (!quiet)
            std::cout << "GIFT quiz saved to: " << output_file << std::endl;
        }
        else if (!interactive)
        {
          std::cout << gift_output << std::endl;
        }
      }
    }
  }
}

void cleanup_files(const std::vector<std::string> &file_ids,
                   const std::string &api_key, const bool quiet = false)
{
  if (file_ids.empty())
    return;

  if (!quiet)
    std::cout << "Starting parallel deletion of " << file_ids.size()
              << " files from Gemini..." << std::endl;

  CURLM *multi_handle = curl_multi_init();
  if (!multi_handle)
  {
    std::cerr << "Failed to initialize CURL multi handle for cleanup"
              << std::endl;
    return;
  }

  std::vector<CURL *> handles(file_ids.size());
  std::vector<std::string> results(file_ids.size());

  // Setup all deletion handles
  for (size_t i = 0; i < file_ids.size(); ++i)
  {
    handles[i] = curl_easy_init();
    if (!handles[i])
    {
      std::cerr << "Failed to initialize CURL handle for deleting "
                << file_ids[i] << std::endl;
      continue;
    }

    std::string url =
        "https://generativelanguage.googleapis.com/v1beta/files/" +
        file_ids[i] + "?key=" + api_key;

    curl_easy_setopt(handles[i], CURLOPT_URL, url.c_str());
    curl_easy_setopt(handles[i], CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(handles[i], CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(handles[i], CURLOPT_WRITEDATA, &results[i]);

    curl_multi_add_handle(multi_handle, handles[i]);
  }

  // Perform all deletions
  int running_handles;
  CURLMcode mc = curl_multi_perform(multi_handle, &running_handles);

  // Wait for all transfers to complete
  while (running_handles > 0)
  {
    fd_set fdread{}, fdwrite{}, fdexcep{};
    int maxfd = -1;
    long curl_timeo = -1;

    curl_multi_timeout(multi_handle, &curl_timeo);
    if (curl_timeo < 0)
      curl_timeo = 1000;

    mc = curl_multi_fdset(multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);
    if (mc != CURLM_OK)
      break;

    struct timeval timeout;
    timeout.tv_sec = curl_timeo / 1000;
    timeout.tv_usec = (curl_timeo % 1000) * 1000;

    int rc = select(maxfd + 1, &fdread, &fdwrite, &fdexcep, &timeout);
    if (rc < 0)
      break;

    curl_multi_perform(multi_handle, &running_handles);
  }

  // Check results and cleanup
  for (size_t i = 0; i < handles.size(); ++i)
  {
    if (handles[i])
    {
      curl_multi_remove_handle(multi_handle, handles[i]);
      curl_easy_cleanup(handles[i]);
    }
  }
  curl_multi_cleanup(multi_handle);

  if (!quiet)
    std::cout << "All " << file_ids.size()
              << " files have been successfully deleted from online storage."
              << std::endl;
}

void print_usage(const char *program_name)
{
  std::cout
      << "Usage: " << program_name
      << " [OPTIONS]"
         R"(

Options:
  --help                Show this help message and exit
  --gemini-api-key KEY  Google Gemini API key
  --interactive         Show GIFT output and ask for approval before saving
  --num-questions N     Number of questions to generate (default: 5)
  --output FILE         Write output to file instead of stdout (for qti format, creates a Common Cartridge .zip)
  --format FMT          Output format: 'gift' (Moodle, default) or 'qti' (Brightspace Common Cartridge)
  --files FILES...      Files to process (can be used multiple times)
  --prompt "TEXT"       Custom query prompt (default: "From both the text and
                        images in the provided files, generate N multiple choice
                        questions formatted according to the provided json
                        schema. Ensure that any code excerpts in the generated
                        questions or answers are surrounded by a pair of
                        backticks. Also ensure each question includes a short
                        title: if a question is based on content from a provided
                        file, start the question title using a short version of
                        the relevant file's title or overall theme. Do not refer
                        to the files provided by an ordinal word, such as
                        "first" or "second". When referring to an image, do this
                        only using one or two words which relate to the content
                        of the image itself; though vary (avoid) this if it
                        might help answer the question. Also generate a short
                        category name (less than 30 characters) that summarizes
                        the topic or subject area of the questions based on the
                        provided context.")

  --gift-context "TEXT" Override the LLM-generated category name with custom
                        text. The category appears at the top of the GIFT
                        output and is used by Moodle to organize questions. If
                        not specified, the LLM generates a short category name
                        based on the content, with a timestamp automatically
                        appended.

  --quiet               Suppress non-error output (except interactive prompts
                        and final GIFT output)

Examples:
)"
         "  "
      << program_name
      << " --files file1.pdf file2.docx --num-questions 10\n"
         "  "
      << program_name
      << " --interactive --files a.pdf --num-questions 5 --files b.txt c.md\n"
         "  "
      << program_name
      << " --prompt \"Generate 7 C++ questions\" --output cpp-quiz.gift\n"
         "  "
      << program_name
      << " --quiet --gemini-api-key abc123 --output quiz.gift --files "
         "../inputs/*.pdf\n"
         "  "
      << program_name
      << R"( --gift-context "Cellular Biology 1" --files cells.pdf --output bio.gift

Environment:
  GEMINI_API_KEY       API key for Google Gemini (if --gemini-api-key not used)

Note: If --prompt is used, it should specify the number of questions to be
      generated. Providing --num-questions too is an error.
)";
}

struct CommandLineArgs
{
  int num_questions = 5;
  std::vector<std::string> files;
  std::string gemini_api_key;
  std::string output_file;
  std::string output_format = "gift";
  std::string custom_prompt;
  std::string context;
  bool interactive = false;
  bool quiet = false;
  bool num_questions_specified = false;
};

CommandLineArgs parse_command_line(int argc, char *argv[])
{
  CommandLineArgs args;

  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];

    if (arg == "--help")
    {
      print_usage(argv[0]);
      exit(0);
    }
    else if (arg == "--num-questions")
    {
      if (i + 1 >= argc)
      {
        throw std::runtime_error("--num-questions requires a value");
      }
      try
      {
        args.num_questions = std::stoi(argv[i + 1]);
        args.num_questions_specified = true;
        if (args.num_questions <= 0)
        {
          throw std::runtime_error("Number of questions must be positive");
        }
      }
      catch (const std::invalid_argument &)
      {
        throw std::runtime_error("Invalid number for --num-questions: " +
                                 std::string(argv[i + 1]));
      }
      ++i; // Skip the value
    }
    else if (arg == "--gemini-api-key")
    {
      if (i + 1 >= argc)
      {
        throw std::runtime_error("--gemini-api-key requires a value");
      }
      args.gemini_api_key = argv[i + 1];
      ++i; // Skip the value
    }
    else if (arg == "--output")
    {
      if (i + 1 >= argc)
      {
        throw std::runtime_error("--output requires a value");
      }
      args.output_file = argv[i + 1];
      ++i; // Skip the value
    }
    else if (arg == "--interactive")
    {
      args.interactive = true;
    }
    else if (arg == "--quiet")
    {
      args.quiet = true;
    }
    else if (arg == "--prompt")
    {
      if (i + 1 >= argc)
      {
        throw std::runtime_error("--prompt requires a value");
      }
      args.custom_prompt = argv[i + 1];
      ++i; // Skip the value
    }
    else if (arg == "--gift-context")
    {
      if (i + 1 >= argc)
      {
        throw std::runtime_error("--gift-context requires a value");
      }
      args.context = argv[i + 1];
      ++i; // Skip the value
    }
    else if (arg == "--format")
    {
      if (i + 1 >= argc)
      {
        throw std::runtime_error("--format requires a value (gift or qti)");
      }
      args.output_format = argv[i + 1];
      if (args.output_format != "gift" && args.output_format != "qti")
      {
        throw std::runtime_error("Invalid format: " + args.output_format + ". Must be 'gift' or 'qti'");
      }
      ++i; // Skip the value
    }
    else if (arg == "--files")
    {
      // Collect all following arguments until next option or end
      ++i;
      while (i < argc && argv[i][0] != '-')
      {
        args.files.push_back(argv[i]);
        ++i;
      }
      --i; // Back up one since the loop will increment
    }
    else if (arg.substr(0, 2) == "--")
    {
      throw std::runtime_error("Unknown option: " + arg);
    }
    else
    {
      throw std::runtime_error("Unexpected argument: " + arg +
                               ". Use --files to specify files.");
    }
  }

  return args;
}

int main(int argc, char *argv[])
{
  if (argc < 2)
  {
    print_usage(argv[0]);
    return 1;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  try
  {
    CommandLineArgs args = parse_command_line(argc, argv);

    if (args.files.empty() && args.custom_prompt.empty())
    {
      std::cerr
          << "Error: No files specified and no custom prompt provided. Use "
             "--files to specify files or --prompt for custom queries.\n"
          << std::endl;
      print_usage(argv[0]);
      return 1;
    }

    if (args.num_questions_specified && !args.custom_prompt.empty())
    {
      std::cerr << "Error: Cannot specify both --num-questions and --prompt. "
                   "The custom prompt should specify the number of questions.\n"
                << std::endl;
      return 1;
    }

    std::string api_key;
    if (!args.gemini_api_key.empty())
    {
      api_key = args.gemini_api_key;
    }
    else
    {
      const char *api_key_env = std::getenv("GEMINI_API_KEY");
      if (!api_key_env)
      {
        std::cerr << "Error: GEMINI_API_KEY environment variable not set and "
                     "--gemini-api-key not provided"
                  << std::endl;
        return 1;
      }
      api_key = api_key_env;
    }

    if (!args.quiet)
    {
      if (args.files.empty())
        std::cout << "Generating questions using a custom prompt." << std::endl;
      else
        std::cout << "Generating " << args.num_questions << " questions from "
                  << args.files.size() << " files." << std::endl;
    }

    std::vector<std::string> file_ids;
    if (!args.files.empty())
    {
      file_ids = upload_files(args.files, api_key, args.quiet);
    }

    try
    {
      run_quiz_generation(args.num_questions, file_ids, api_key,
                          args.output_file, args.interactive, args.quiet,
                          args.custom_prompt, args.context, args.output_format);
    }
    catch (...)
    {
      cleanup_files(file_ids, api_key, args.quiet);
      throw; // Re-throw exception (e.g. 503 model overloaded) after cleanup
    }

    // Clean up uploaded files on the success path too (the catch above
    // only runs on failure). cleanup_files is a no-op for empty file_ids.
    cleanup_files(file_ids, api_key, args.quiet);
  }
  catch (const std::exception &e)
  {
    std::cerr << "Error: " << e.what() << std::endl;
    curl_global_cleanup();
    return 1;
  }

  curl_global_cleanup();
  return 0;
}
