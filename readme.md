# AI Quiz Generator

This command-line tool can quickly generate multiple choice questions (MCQs) for Virtual Learning Environments (VLEs) using a Large Language Model (LLM). It supports exporting to **Moodle (GIFT format)** and **Brightspace (QTI / Common Cartridge 1.3 format)**. 

The Google Gemini 2.5 Flash LLM is configured as the default provider, but Gemini 2.5 Pro can easily be used instead. Structured output is ensured by a JSON Schema. A Google Gemini API key is required.

If you would like to cite this work, please use the following reference:

*"Scaling assessment innovation: from immediate feedback MCQs to automated question generation with LLMs"*  
Gurney, T. & Keir, P., 7 Jan 2026, Proceedings of 10th Computing Education Practice CEP: 8th January 2026, Durham University UK. Bradley, S. & Southern, K. (eds.). New York: Association for Computing Machinery (ACM), p. 13-16 4 p. (ACM International Conference Proceeding Series). https://doi.org/10.1145/3772338.3772353

## Building

The AI Quiz Generator relies on libcurl for network transfer via HTTPS; and the Nlohmann JSON library for parsing and generation of JSON packets. The `miniz` compression library is used for packaging Brightspace zips, but it is vendored directly into the repository so you don't need to install it.

* [libcurl](https://curl.se/libcurl)
* [Nlohmann JSON](https://github.com/nlohmann/json)
* [miniz](https://github.com/richgel999/miniz) (vendored)

Your favourite package manager can install the network/JSON dependencies. Use CMake to configure; then build the `ai-quiz-gen` executable.

**Ubuntu (Debian)**
```bash
sudo apt-get install libcurl4-openssl-dev nlohmann-json3-dev
cmake ..
make
```

**MacOS**
```bash
brew install curl nlohmann-json
cmake ..
make
```

## Example Usage

Before you run the generator, you need an API key from [Google AI Studio](https://aistudio.google.com). You can provide this key via the `GEMINI_API_KEY` environment variable, or by passing the `--gemini-api-key` command-line flag.

### 1. Generating for Moodle (GIFT)
By default, the tool outputs Moodle GIFT format.
```bash
./ai-quiz-gen --files inputs/reading-material.pdf --num-questions 10 --output quiz.gift
```

### 2. Generating for Brightspace (Common Cartridge 1.3)
You can instruct the tool to generate a Brightspace-compatible `.zip` package by passing `--format qti`.
```bash
./ai-quiz-gen --format qti --files inputs/reading-material.pdf --num-questions 10 --output brightspace_quiz.zip
```
> **IMPORTANT: Brightspace Import Instructions**
> When importing the generated `.zip` package into Brightspace, you **must** upload it via the **Course Admin Panel -> Import/Export/Copy Components**. 
> Do *not* try to upload the `.zip` file directly through the Quizzes menu, as Brightspace requires the package to be ingested as a Common Cartridge component.

### Usage Options

```text
$ ./ai-quiz-gen
Usage: ./ai-quiz-gen [OPTIONS]

Options:
  --help                Show this help message and exit
  --gemini-api-key KEY  Google Gemini API key
  --interactive         Show GIFT output and ask for approval before saving
  --num-questions N     Number of questions to generate (default: 5)
  --output FILE         Write output to file instead of stdout (for qti format, creates a Common Cartridge .zip)
  --format FMT          Output format: 'gift' (Moodle, default) or 'qti' (Brightspace Common Cartridge)
  --files FILES...      Files to process (can be used multiple times)
  --prompt "TEXT"       Custom query prompt (default prompts are provided)
  --gift-context "TEXT" Override the LLM-generated category name/quiz title.
  --quiet               Suppress non-error output

Examples:
  ./ai-quiz-gen --format qti --files chapter1.pdf --num-questions 5 --output ch1_quiz.zip
  ./ai-quiz-gen --prompt "Generate 7 C++ questions" --output cpp-quiz.gift
  ./ai-quiz-gen --quiet --gemini-api-key abc123 --output quiz.gift --files ../inputs/*.pdf
```

## Contributing
Contributions are welcome. Please create or comment on an issue; or submit a pull request.

## License
MIT License
