#include <tree_sitter/parser.h>
#include <algorithm>
#include <vector>
#include <string>
#include <cwctype>
#include "tag.h"

namespace {

using std::vector;
using std::string;

enum TokenType {
  START_TAG_NAME,
  START_RAW_TAG_NAME,
  END_TAG_NAME,
  ERRONEOUS_END_TAG_NAME,
  SELF_CLOSING_TAG_DELIMITER,
  IMPLICIT_END_TAG,
  RAW_TEXT,
  COMMENT
};

struct Scanner {
  Scanner() {}

  unsigned serialize(char *buffer) {
    unsigned i = 0;
    for (Tag &tag : tags) {
      buffer[i] = static_cast<char>(tag.type);
      i++;

      if (tag.type == CUSTOM) {
        buffer[i++] = tag.custom_tag_name.size();
        for (char c : tag.custom_tag_name) {
          buffer[i++] = c;
        }
      }
    }

    return i;
  }

  void deserialize(const char *buffer, unsigned length) {
    tags.clear();

    unsigned i = 0;
    while (i < length) {
      Tag tag { static_cast<TagType>(buffer[i]), "" };
      i++;
      if (tag.type == CUSTOM) {
        tag.custom_tag_name.resize(buffer[i++]);
        for (unsigned j = 0; j < tag.custom_tag_name.size(); j++) {
          tag.custom_tag_name[j] = buffer[i++];
        }
      }
      tags.push_back(tag);
    }
  }

  string scan_tag_name(TSLexer *lexer) {
    string tag_name;
    while (iswalnum(lexer->lookahead) || lexer->lookahead == '-' || lexer->lookahead == ':') {
      tag_name += towupper(lexer->lookahead);
      lexer->advance(lexer, false);
    }
    return tag_name;
  }

  bool scan_comment(TSLexer *lexer) {
    if (lexer->lookahead != '-') return false;
    lexer->advance(lexer, false);
    if (lexer->lookahead != '-') return false;
    lexer->advance(lexer, false);

    unsigned dashes = 0;
    auto c = lexer->lookahead;
    while (c) {
      switch (c) {
        case '-':
          ++dashes;
          break;
        case '>':
          if (dashes >= 2) {
            lexer->result_symbol = COMMENT;
            lexer->advance(lexer, false);
            lexer->mark_end(lexer);
            return true;
          }
          break;
        default:
          dashes = 0;
      }
      lexer->advance(lexer, false);
      c = lexer->lookahead;
    }
    return false;
  }

  bool scan_raw_text(TSLexer *lexer) {
    if (!tags.size()) return false;

    lexer->mark_end(lexer);

    const string &end_delimiter = tags.back().type == SCRIPT
      ? "</script"
      : "</style";

    unsigned delimiter_index = 0;
    while (lexer->lookahead) {
      if (lexer->lookahead == end_delimiter[delimiter_index]) {
        delimiter_index++;
        if (delimiter_index == end_delimiter.size()) break;
      } else {
        delimiter_index = 0;
        lexer->mark_end(lexer);
      }
      lexer->advance(lexer, false);
    }

    lexer->result_symbol = RAW_TEXT;
    return true;
  }

  bool scan_implicit_end_tag(TSLexer *lexer) {
    Tag *parent = tags.empty() ? nullptr : &tags.back();

    bool is_closing_tag = false;
    if (lexer->lookahead == '/') {
      is_closing_tag = true;
      lexer->advance(lexer, false);
    } else {
      if (parent && parent->is_void()) {
        tags.pop_back();
        lexer->result_symbol = IMPLICIT_END_TAG;
        return true;
      }
    }

    auto tag_name = scan_tag_name(lexer);
    if (tag_name.empty()) return false;

    Tag next_tag = Tag::for_name(tag_name);

    if (is_closing_tag) {
      // The tag correctly closes the topmost element on the stack
      if (next_tag == tags.back()) return false;

      // Otherwise, dig deeper and queue implicit end tags (to be nice in
      // the case of malformed HTML)
      if (std::find(tags.begin(), tags.end(), next_tag) != tags.end()) {
        tags.pop_back();
        lexer->result_symbol = IMPLICIT_END_TAG;
        return true;
      }
    } else if (parent && !parent->can_contain(next_tag)) {
      tags.pop_back();
      lexer->result_symbol = IMPLICIT_END_TAG;
      return true;
    }

    return false;
  }

  bool scan_start_tag_name(TSLexer *lexer) {
    auto tag_name = scan_tag_name(lexer);
    if (tag_name.empty()) return false;
    Tag tag = Tag::for_name(tag_name);
    tags.push_back(tag);
    if (tag.is_raw()) {
      lexer->result_symbol = START_RAW_TAG_NAME;
    } else {
      lexer->result_symbol = START_TAG_NAME;
    }
    return true;
  }

  bool scan_end_tag_name(TSLexer *lexer) {
    auto tag_name = scan_tag_name(lexer);
    if (tag_name.empty()) return false;
    Tag tag = Tag::for_name(tag_name);
    if (!tags.empty() && tags.back() == tag) {
      tags.pop_back();
      lexer->result_symbol = END_TAG_NAME;
    } else {
      lexer->result_symbol = ERRONEOUS_END_TAG_NAME;
    }
    return true;
  }

  bool scan_self_closing_tag_delimiter(TSLexer *lexer) {
    lexer->advance(lexer, false);
    if (lexer->lookahead == '>') {
      lexer->advance(lexer, false);
      tags.pop_back();
      lexer->result_symbol = SELF_CLOSING_TAG_DELIMITER;
      return true;
    }
    return false;
  }

  bool scan(TSLexer *lexer, const bool *valid_symbols) {
    while (iswspace(lexer->lookahead)) {
      lexer->advance(lexer, true);
    }

    if (valid_symbols[RAW_TEXT] && !valid_symbols[START_TAG_NAME] && !valid_symbols[END_TAG_NAME]) {
      return scan_raw_text(lexer);
    }

    switch (lexer->lookahead) {
      case '<':
        lexer->mark_end(lexer);
        lexer->advance(lexer, false);

        if (lexer->lookahead == '!') {
          lexer->advance(lexer, false);
          return scan_comment(lexer);
        }

        if (valid_symbols[IMPLICIT_END_TAG]) {
          return scan_implicit_end_tag(lexer);
        }
        break;

      case '/':
        if (valid_symbols[SELF_CLOSING_TAG_DELIMITER]) {
          return scan_self_closing_tag_delimiter(lexer);
        }
        break;

      default:
        if (valid_symbols[START_TAG_NAME] || valid_symbols[END_TAG_NAME]) {
          return valid_symbols[START_TAG_NAME]
            ? scan_start_tag_name(lexer)
            : scan_end_tag_name(lexer);
        }
    }

    return false;
  }

  vector<Tag> tags;
};

}

extern "C" {

void *tree_sitter_html_external_scanner_create() {
  return new Scanner();
}

bool tree_sitter_html_external_scanner_scan(void *payload, TSLexer *lexer,
                                            const bool *valid_symbols) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  return scanner->scan(lexer, valid_symbols);
}

unsigned tree_sitter_html_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  return scanner->serialize(buffer);
}

void tree_sitter_html_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  scanner->deserialize(buffer, length);
}

void tree_sitter_html_external_scanner_destroy(void *payload) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  delete scanner;
}

}
