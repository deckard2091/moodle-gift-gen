// gift-validator-example.cpp — example program for gift-validator.
//
// Usage:
//   gift-validator-example <input.gift> [--valid <output.gift>] [--invalid <output.gift>]
//
// Parses <input.gift> and prints a diagnostic report to stdout.
// Exits with code 0 if all questions are valid, 1 if any are not, 2 on error.
//
//   --valid  <file>   write only the valid questions to <file>
//   --invalid <file>  write only the invalid questions to <file>
//
// Both flags are optional and may be combined.
//
// Build:
//   g++ -std=c++17 -O2 gift-validator.cpp gift-validator-example.cpp -o gift-validator-example

#include "gift-validator.hpp"

#include <iostream>
#include <string>

int main(int argc, char *argv[])
{
  if (argc < 2)
  {
    std::cerr << "Usage: " << argv[0]
              << " <input.gift> [--valid <output.gift>] [--invalid <output.gift>]\n";
    return 2;
  }

  const std::string input_path = argv[1];
  std::string valid_path, invalid_path;

  for (int i = 2; i < argc; ++i)
  {
    std::string arg = argv[i];
    if (arg == "--valid" && i + 1 < argc)
      valid_path = argv[++i];
    else if (arg == "--invalid" && i + 1 < argc)
      invalid_path = argv[++i];
    else
    {
      std::cerr << "Unknown argument: " << arg << "\n"
                << "Usage: " << argv[0]
                << " <input.gift> [--valid <output.gift>] [--invalid <output.gift>]\n";
      return 2;
    }
  }

  gift::ParseResult result = gift::parse_file(input_path);

  if (result.questions.empty())
  {
    std::cerr << "Error: could not read '" << input_path
              << "' or file is empty.\n";
    return 2;
  }

  std::cout << gift::report(result);

  if (!valid_path.empty())
  {
    if (gift::write_valid_file(result, valid_path))
      std::cout << "Valid questions written to: " << valid_path << "\n";
    else
    {
      std::cerr << "Error: could not write to '" << valid_path << "'.\n";
      return 2;
    }
  }

  if (!invalid_path.empty())
  {
    if (gift::write_invalid_file(result, invalid_path))
      std::cout << "Invalid questions written to: " << invalid_path << "\n";
    else
    {
      std::cerr << "Error: could not write to '" << invalid_path << "'.\n";
      return 2;
    }
  }

  return result.invalid_count() > 0 ? 1 : 0;
}
