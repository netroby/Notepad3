/**
 * @file  OnigmoRegExEngine.cxx
 * @brief integrate Onigmo regex engine for Scintilla library
 *        (Scintilla Lib is copyright 1998-2017 by Neil Hodgson <neilh@scintilla.org>)
 *
 *        uses Onigmo - Regular Expression Engine (v6.1.3) (onigmo.h) - https://github.com/k-takata/Onigmo
 *
 *   Onigmo is a regular expressions library forked from Oniguruma (https://github.com/kkos/oniguruma). 
 *   It focuses to support new expressions like \K, \R, (?(cond)yes|no) and etc. which are supported in Perl 5.10+.
 *   Since Onigmo is used as the default regexp library of Ruby 2.0 or later, many patches are backported from Ruby 2.x.
 *
 *   See also the Wiki page: https://github.com/k-takata/Onigmo/wiki
 *
 *
 * @autor Rainer Kottenhoff (RaiKoHoff)
 *
 * TODO: add interface to onig_scan() API (mark occ, hyperlink)
 */

#ifdef SCI_OWNREGEX

#include <stdlib.h>
#include <string>
#include <vector>
#include <sstream>

#define VC_EXTRALEAN 1
#include <windows.h>

#pragma warning( push )
#pragma warning( disable : 4996 )   // Scintilla's "unsafe" use of std::copy() (SplitVector.h)
//                                  // or use -D_SCL_SECURE_NO_WARNINGS preprocessor define

#include "Platform.h"
#include "Scintilla.h"
#include "ILexer.h"
#include "SplitVector.h"
#include "Partitioning.h"
#include "CellBuffer.h"
#include "CaseFolder.h"
#include "RunStyles.h"
#include "Decoration.h"
#include "CharClassify.h"
#include "Document.h"
// ---------------------------------------------------------------
#include "onigmo.h"   // Onigmo - Regular Expression Engine (v6.1.3)
// ---------------------------------------------------------------

using namespace Scintilla;

#define SciPos(pos)    static_cast<Sci::Position>(pos)
#define SciLn(line)    static_cast<Sci::Line>(line)
#define SciPosExt(pos) static_cast<Sci_Position>(pos)

#define Cast2long(n)   static_cast<long>(n)


// ============================================================================
// ***   Oningmo configuration   ***
// ============================================================================

const OnigEncoding g_pOnigEncodingType = ONIG_ENCODING_UTF8;  // ONIG_ENCODING_ASCII;
static OnigEncoding g_UsedEncodingsTypes[] = { g_pOnigEncodingType };

// ============================================================================
// ============================================================================

class OnigmoRegExEngine : public RegexSearchBase
{
public:

  explicit OnigmoRegExEngine(CharClassify* charClassTable)
    : m_RegExprStrg()
    , m_OnigSyntax(*ONIG_SYNTAX_PERL)
    , m_CmplOptions(ONIG_OPTION_DEFAULT)
    , m_RegExpr(nullptr)
    , m_Region({0,0,nullptr,nullptr,nullptr})
    , m_ErrorInfo()
    , m_MatchPos(ONIG_MISMATCH)
    , m_MatchLen(0)
    , m_SubstBuffer()
  {
    m_OnigSyntax.op |= ONIG_SYN_OP_ESC_LTGT_WORD_BEGIN_END;
    onig_initialize(g_UsedEncodingsTypes, _ARRAYSIZE(g_UsedEncodingsTypes));
    onig_region_init(&m_Region);
  }

  virtual ~OnigmoRegExEngine()
  {
    onig_region_free(&m_Region, 0);
    onig_free(m_RegExpr);
    onig_end();
  }

  virtual long FindText(Document* doc, Sci::Position minPos, Sci::Position maxPos, const char* pattern,
                        bool caseSensitive, bool word, bool wordStart, int flags, Sci::Position* length) override;

  virtual const char* SubstituteByPosition(Document* doc, const char* text, Sci::Position* length) override;


  const OnigRegion& GetRegion() const { return m_Region; };

private:

  std::string& translateRegExpr(std::string& regExprStr, bool wholeWord, bool wordStart, int eolMode, OnigOptionType& rxOptions);

  std::string& convertReplExpr(std::string& replStr);

  //void regexFindAndReplace(std::string& inputStr_inout, const std::string& patternStr, const std::string& replStr);

private:

  std::string m_RegExprStrg;

  OnigSyntaxType  m_OnigSyntax;
  OnigOptionType  m_CmplOptions;
  OnigRegex       m_RegExpr;
  OnigRegion      m_Region;

  char            m_ErrorInfo[ONIG_MAX_ERROR_MESSAGE_LEN];

  Sci::Position   m_MatchPos;
  Sci::Position   m_MatchLen;

public:
  std::string m_SubstBuffer;

};
// ============================================================================


RegexSearchBase *Scintilla::CreateRegexSearch(CharClassify *charClassTable)
{
  return new OnigmoRegExEngine(charClassTable);
}

// ============================================================================



// ============================================================================
//   Some Helpers
// ============================================================================


/******************************************************************************
*
*  UnSlash functions
*  Mostly taken from SciTE, (c) Neil Hodgson, http://www.scintilla.org
*
/

/**
* Is the character an octal digit?
*/
static bool IsOctalDigit(char ch)
{
  return ch >= '0' && ch <= '7';
}
// ----------------------------------------------------------------------------

/**
* If the character is an hexa digit, get its value.
*/
static int GetHexDigit(char ch)
{
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  return -1;
}
// ----------------------------------------------------------------------------


static void replaceAll(std::string& source, const std::string& from, const std::string& to)
{
  std::string newString;
  newString.reserve(source.length() * 2);  // avoids a few memory allocations

  std::string::size_type lastPos = 0;
  std::string::size_type findPos;

  while (std::string::npos != (findPos = source.find(from, lastPos))) {
    newString.append(source, lastPos, findPos - lastPos);
    newString += to;
    lastPos = findPos + from.length();
  }
  // Care for the rest after last occurrence
  newString += source.substr(lastPos);

  source.swap(newString);
}
// ----------------------------------------------------------------------------


/**
 * Find text in document, supporting both forward and backward
 * searches (just pass minPos > maxPos to do a backward search)
 * Has not been tested with backwards DBCS searches yet.
 */
long OnigmoRegExEngine::FindText(Document* doc, Sci::Position minPos, Sci::Position maxPos, const char *pattern,
                                 bool caseSensitive, bool word, bool wordStart, int searchFlags, Sci::Position *length)
{
  if (!(pattern && (strlen(pattern) > 0))) {
    *length = 0;
    return Cast2long(-1);
  }

  Sci::Position docLen = SciPos(doc->Length());

  const bool findForward = (minPos <= maxPos);
  const int increment = findForward ? 1 : -1;

  // Range endpoints should not be inside DBCS characters, but just in case, move them.
  minPos = doc->MovePositionOutsideChar(minPos, increment, false);
  maxPos = doc->MovePositionOutsideChar(maxPos, increment, false);

  Sci::Position rangeBeg = (findForward) ? minPos : maxPos;
  Sci::Position rangeEnd = (findForward) ? maxPos : minPos;
  Sci::Position rangeLen = (rangeEnd - rangeBeg);


  // -----------------------------
  // --- Onigmo Engine Options ---
  // -----------------------------

  // fixed options
  OnigOptionType onigmoOptions = ONIG_OPTION_DEFAULT;

  // OFF: not wanted options in Notepad3
  ONIG_OPTION_OFF(onigmoOptions, ONIG_OPTION_EXTEND);
  ONIG_OPTION_OFF(onigmoOptions, ONIG_OPTION_ASCII_RANGE);

  // ONIG_OPTION_DOTALL == ONIG_OPTION_MULTILINE
  if (searchFlags & SCFIND_DOT_MATCH_ALL) {
    ONIG_OPTION_ON(onigmoOptions, ONIG_OPTION_DOTALL);
  }
  else {
    ONIG_OPTION_OFF(onigmoOptions, ONIG_OPTION_DOTALL);
  }


  //ONIG_OPTION_ON(onigmoOptions, ONIG_OPTION_SINGLELINE);
  ONIG_OPTION_ON(onigmoOptions, ONIG_OPTION_NEGATE_SINGLELINE);

  ONIG_OPTION_ON(onigmoOptions, ONIG_OPTION_CAPTURE_GROUP);

  // dynamic options
  ONIG_OPTION_ON(onigmoOptions, caseSensitive ? ONIG_OPTION_NONE : ONIG_OPTION_IGNORECASE);
  ONIG_OPTION_ON(onigmoOptions, (rangeBeg != 0) ? ONIG_OPTION_NOTBOL : ONIG_OPTION_NONE);
  ONIG_OPTION_ON(onigmoOptions, (rangeEnd != docLen) ? ONIG_OPTION_NOTEOL : ONIG_OPTION_NONE);

  std::string sRegExprStrg = translateRegExpr(std::string(pattern), word, wordStart, doc->eolMode, onigmoOptions);

  bool bReCompile = (m_RegExpr == nullptr) || (m_CmplOptions != onigmoOptions) || (m_RegExprStrg.compare(sRegExprStrg) != 0);

  if (bReCompile) {
    m_RegExprStrg.clear();
    m_RegExprStrg = sRegExprStrg;
    m_CmplOptions = onigmoOptions;
    m_ErrorInfo[0] = '\0';
    try {
      OnigErrorInfo einfo;

      onig_region_free(&m_Region, 0);

      int res = onig_new(&m_RegExpr, (UChar*)m_RegExprStrg.c_str(), (UChar*)(m_RegExprStrg.c_str() + m_RegExprStrg.length()),
                         m_CmplOptions, g_pOnigEncodingType, &m_OnigSyntax, &einfo);
      if (res != 0) {
        onig_error_code_to_str((UChar*)m_ErrorInfo, res, &einfo);
        return Cast2long(-2);   // -1 is normally used for not found, -2 is used here for invalid regex
      }

      onig_region_init(&m_Region);
    }
    catch (...) {
      return Cast2long(-2);
    }
  }

  m_MatchPos = SciPos(ONIG_MISMATCH); // not found
  m_MatchLen = SciPos(0);

  // ---  search document range for pattern match   ---
  UChar* docBegPtr = (UChar*)doc->RangePointer(0, docLen);
  UChar* docSEndPtr = (UChar*)doc->RangePointer(docLen, 0);
  UChar* rangeBegPtr = (UChar*)doc->RangePointer(rangeBeg, rangeLen);
  UChar* rangeEndPtr = (UChar*)doc->RangePointer(rangeEnd, 0);

  OnigPosition result = ONIG_MISMATCH;
  try {
    if (findForward)
      result = onig_search(m_RegExpr, docBegPtr, docSEndPtr, rangeBegPtr, rangeEndPtr, &m_Region, onigmoOptions);
    else //                                                              X                                    //
      result = onig_search(m_RegExpr, docBegPtr, docSEndPtr, rangeEndPtr, rangeBegPtr, &m_Region, onigmoOptions);
  }
  catch (...) {
    return Cast2long(-3);  // -1 is normally used for not found, -3 is used here for exception
  }

  if (result < ONIG_MISMATCH) {
    onig_error_code_to_str((UChar*)m_ErrorInfo, result);
    return Cast2long(-3);
  }

  if ((result >= 0) && (rangeBegPtr <= rangeEndPtr)) 
  {
    m_MatchPos = SciPos(result); //SciPos(m_Region.beg[0]);
    m_MatchLen = SciPos(m_Region.end[0] - result);
  }

  //NOTE: potential 64-bit-size issue at interface here:
  *length = m_MatchLen;
  return Cast2long(m_MatchPos);
}
// ============================================================================



////static int GrpNameCallback(const UChar* name, const UChar* name_end, 
////  int ngroup_num, int* group_nums, regex_t* reg, void* arg)
////{
////  OnigmoRegExEngine* pRegExInstance = dynamic_cast<OnigmoRegExEngine*>(arg);
////
////  const OnigRegion& region = pRegExInstance->GetRegion();
////
////  for (int i = 0; i < ngroup_num; i++) 
////  {
////    int grpNum = group_nums[i];
////    
////    int ref = onig_name_to_backref_number(reg, name, name_end, &region);
////
////    if (ref == grpNum) {
////
////      Sci::Position rBeg = SciPos(region.beg[grpNum]);
////      Sci::Position len = SciPos(region.end[grpNum] - rBeg);
////
////      (pRegExInstance->m_SubstBuffer).append(doc->RangePointer(rBeg, len), (size_t)len);
////
////    }
////  }
////  return 0;  /* 0: continue */
////}
////   called by:  int r = onig_foreach_name(m_RegExpr, GrpNameCallback, (void*)this);


// ============================================================================



const char* OnigmoRegExEngine::SubstituteByPosition(Document* doc, const char* text, Sci::Position* length)
{

  if (m_MatchPos < 0) {
    *length = SciPos(-1);
    return nullptr;
  }

  std::string rawReplStrg = convertReplExpr(std::string(text, *length));

  m_SubstBuffer.clear();

  //TODO: allow for arbitrary number of grups/regions

  for (size_t j = 0; j < rawReplStrg.length(); j++) 
  {
    bool bReplaced = false;
    if ((rawReplStrg[j] == '$') || (rawReplStrg[j] == '\\'))
    {
      if ((rawReplStrg[j + 1] >= '0') && (rawReplStrg[j + 1] <= '9'))
      {
        int grpNum = rawReplStrg[j + 1] - '0';
        if (grpNum < m_Region.num_regs)
        {
          Sci::Position rBeg = SciPos(m_Region.beg[grpNum]);
          Sci::Position len = SciPos(m_Region.end[grpNum] - rBeg);

          m_SubstBuffer.append(doc->RangePointer(rBeg, len), (size_t)len);
        }
        bReplaced = true;
        ++j;
      }
      else if (rawReplStrg[j] == '$')
      {
        size_t k = ((rawReplStrg[j + 1] == '+') && (rawReplStrg[j + 2] == '{')) ? (j + 3) : ((rawReplStrg[j + 1] == '{') ? (j + 2) : 0);
        if (k > 0) {
          // named group replacemment
          UChar* name_beg = (UChar*)&(rawReplStrg[k]);
          while (rawReplStrg[k] &&  IsCharAlphaNumericA(rawReplStrg[k])) { ++k; }
          if (rawReplStrg[k] == '}')
          {
            int grpNum = onig_name_to_backref_number(m_RegExpr, name_beg, (UChar*)&(rawReplStrg[k]), &m_Region);
            if ((grpNum >= 0) && (grpNum < m_Region.num_regs))
            {
              Sci::Position rBeg = SciPos(m_Region.beg[grpNum]);
              Sci::Position len = SciPos(m_Region.end[grpNum] - rBeg);

              m_SubstBuffer.append(doc->RangePointer(rBeg, len), (size_t)len);
            }
            bReplaced = true;
            j = k;
          }
        }
      }
      else if ((rawReplStrg[j] == '\\') && (rawReplStrg[j+1] == '\\')){
        m_SubstBuffer.push_back('\\');
        bReplaced = true;
        ++j;
      }
    }
    if (!bReplaced) { m_SubstBuffer.push_back(rawReplStrg[j]); }
  }

  //NOTE: potential 64-bit-size issue at interface here:
  *length = SciPos(m_SubstBuffer.length());
  return m_SubstBuffer.c_str();
}
// ============================================================================



// ============================================================================
//
// private methods
//
// ============================================================================

/*
void OnigmoRegExEngine::regexFindAndReplace(std::string& inputStr_inout, const std::string& patternStr, const std::string& replStr)
{
  OnigRegex       oRegExpr;
  OnigRegion      oRegion;

  const UChar* pattern = (UChar*)patternStr.c_str();

  OnigErrorInfo einfo;
  int res = onig_new(&oRegExpr, pattern, pattern + strlen((char*)pattern),
    ONIG_OPTION_DEFAULT, ONIG_ENCODING_ASCII, ONIG_SYNTAX_DEFAULT, &einfo);

  if (res != ONIG_NORMAL) { return; }
  
  const UChar* strg = (UChar*)inputStr_inout.c_str();
  const UChar* start = strg;
  const UChar* end = (start + patternStr.length());
  const UChar* range = end;

  onig_region_init(&oRegion);

  OnigPosition pos = onig_search(oRegExpr, strg, end, start, range, &oRegion, ONIG_OPTION_DEFAULT);

  if (pos >= 0) 
  {
    std::string replace = replStr; // copy
    for (int i = 1; i < oRegion.num_regs; i++) {
      std::ostringstream nr;
      nr << R"(\)" << i;
      std::string regio((char*)(strg + oRegion.beg[i]), (oRegion.end[i] - oRegion.beg[i]));
      replaceAll(replace, nr.str(), regio);
    }
    inputStr_inout.replace(oRegion.beg[0], (oRegion.end[0] - oRegion.beg[0]), replace);
  }

  onig_region_free(&oRegion, 0);
  onig_free(oRegExpr);
}
// ----------------------------------------------------------------------------
*/



std::string& OnigmoRegExEngine::translateRegExpr(std::string& regExprStr, bool wholeWord, bool wordStart, int eolMode, OnigOptionType& rxOptions)
{
  std::string	tmpStr;
  bool bUseTmpStrg = false;

  if (wholeWord || wordStart) {      // push '\b' at the begin of regexpr
    tmpStr.push_back('\\');
    tmpStr.push_back('b');
    tmpStr.append(regExprStr);
    if (wholeWord) {               // push '\b' at the end of regexpr
      tmpStr.push_back('\\');
      tmpStr.push_back('b');
    }
    replaceAll(tmpStr, ".", R"(\w)");
    bUseTmpStrg = true;
  }
  else {
    tmpStr.append(regExprStr);
  }

  // Onigmo supports LTGT word boundary by: ONIG_SYN_OP_ESC_LTGT_WORD_BEGIN_END
  //
  //~replaceAll(tmpStr, R"(\<)", R"((?<!\w)(?=\w))");  // word begin
  //~replaceAll(tmpStr, R"(\(?<!\w)(?=\w))", R"(\\<)"); // esc'd
  //~replaceAll(tmpStr, R"(\>)", R"((?<=\w)(?!\w))"); // word end
  //~replaceAll(tmpStr, R"(\(?<=\w)(?!\w))", R"(\\>)"); // esc'd
  //~bUseTmpStrg = true;


  // EOL modes
  switch (eolMode) {
  case SC_EOL_LF:
  case SC_EOL_CR:
    ONIG_OPTION_OFF(rxOptions, ONIG_OPTION_NEWLINE_CRLF);
    break;

  case SC_EOL_CRLF:
    ONIG_OPTION_ON(rxOptions, ONIG_OPTION_NEWLINE_CRLF);
    break;
  }

  if (bUseTmpStrg) { std::swap(regExprStr, tmpStr); }

  return regExprStr;
}
// ----------------------------------------------------------------------------



std::string& OnigmoRegExEngine::convertReplExpr(std::string& replStr)
{
  std::string	tmpStr;
  for (size_t i = 0; i < replStr.length(); ++i) {
    char ch = replStr[i];
    if (ch == '\\') {
      ch = replStr[++i]; // next char
      if (ch >= '1' && ch <= '9') {
        // former behavior convenience: 
        // change "\\<n>" to deelx's group reference ($<n>)
        tmpStr.push_back('$');
      }
      switch (ch) {
        // check for escape seq:
      case 'a':
        tmpStr.push_back('\a');
        break;
      case 'b':
        tmpStr.push_back('\b');
        break;
      case 'f':
        tmpStr.push_back('\f');
        break;
      case 'n':
        tmpStr.push_back('\n');
        break;
      case 'r':
        tmpStr.push_back('\r');
        break;
      case 't':
        tmpStr.push_back('\t');
        break;
      case 'v':
        tmpStr.push_back('\v');
        break;
      case '\\':
        tmpStr.push_back('\\'); // preserve escd "\"
        tmpStr.push_back('\\'); 
        break;
      case 'x':
      case 'u':
        {
          bool bShort = (ch == 'x');
          char buf[8] = { '\0' };
          char *pch = buf;
          WCHAR val[2] = L"";
          int hex;
          val[0] = 0;
          ++i;
          hex = GetHexDigit(replStr[i]);
          if (hex >= 0) {
            ++i;
            val[0] = (WCHAR)hex;
            hex = GetHexDigit(replStr[i]);
            if (hex >= 0) {
              ++i;
              val[0] *= 16;
              val[0] += (WCHAR)hex;
              if (!bShort) {
                hex = GetHexDigit(replStr[i]);
                if (hex >= 0) {
                  ++i;
                  val[0] *= 16;
                  val[0] += (WCHAR)hex;
                  hex = GetHexDigit(replStr[i]);
                  if (hex >= 0) {
                    ++i;
                    val[0] *= 16;
                    val[0] += (WCHAR)hex;
                  }
                }
              }
            }
            if (val[0]) {
              val[1] = 0;
              WideCharToMultiByte(CP_UTF8, 0, val, -1, buf, ARRAYSIZE(val), nullptr, nullptr);
              tmpStr.push_back(*pch++);
              while (*pch)
                tmpStr.push_back(*pch++);
            }
            else
              tmpStr.push_back(ch); // unknown ctrl seq
          }
          else
            tmpStr.push_back(ch); // unknown ctrl seq
        }
        break;

      default:
        tmpStr.push_back(ch); // unknown ctrl seq
        break;
      }
    }
    else {
      tmpStr.push_back(ch);
    }
  } //for

  std::swap(replStr,tmpStr);
  return replStr;
}
// ============================================================================

#pragma warning( pop )

#endif //SCI_OWNREGEX
