/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * This file is part of the libabw project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <string.h>

#include <set>
#include <stack>
#include <utility>

#include <libxml/xmlIO.h>
#include <libxml/xmlstring.h>
#include <librevenge-stream/librevenge-stream.h>
#include <boost/spirit/include/qi.hpp>
#include "ABWParser.h"
#include "ABWContentCollector.h"
#include "ABWStylesCollector.h"
#include "libabw_internal.h"
#include "ABWXMLHelper.h"
#include "ABWXMLTokenMap.h"


namespace libabw
{

namespace
{

static bool findBool(const std::string &str, bool &res)
{
  using namespace boost::spirit::qi;

  if (str.empty())
    return false;

  symbols<char, bool> bools;
  bools.add
  ("true", true)
  ("false", false)
  ("yes", true)
  ("no", false)
  ;

  auto it = str.cbegin();
  return phrase_parse(it, str.cend(), no_case[bools], space, res) && it == str.cend();
}

// small function needed to call the xml BAD_CAST on a char const *
static xmlChar *call_BAD_CAST_OnConst(char const *str)
{
  return BAD_CAST(const_cast<char *>(str));
}

/** try to find the parent's level corresponding to a level with some id
    and use its original id to define the list id.

    Seen corresponds to the list of level that we have already examined,
    it is used to check also for loop
  */
static int _findAndUpdateListElementId(std::map<int, std::shared_ptr<ABWListElement>> &listElements, int id, std::set<int> &seen)
{
  if (listElements.find(id)==listElements.end() || !listElements.find(id)->second)
    return 0;
  const std::shared_ptr<ABWListElement> &tmpElement= listElements.find(id)->second;
  if (tmpElement->m_listId)
    return tmpElement->m_listId;
  if (seen.find(id)!=seen.end())
  {
    // oops, this means that we have a loop
    tmpElement->m_parentId=0;
  }
  else
    seen.insert(id);
  if (!tmpElement->m_parentId)
  {
    tmpElement->m_listId=id;
    return id;
  }
  tmpElement->m_listId=_findAndUpdateListElementId(listElements, tmpElement->m_parentId, seen);
  return tmpElement->m_listId;
}

/** try to update the final list id for each list elements */
static void updateListElementIds(std::map<int, std::shared_ptr<ABWListElement>> &listElements)
{
  std::set<int> seens;
  for (const auto &elem : listElements)
  {
    if (!elem.second) continue;
    _findAndUpdateListElementId(listElements, elem.first, seens);
  }
}

} // anonymous namespace

struct ABWParserState
{
  ABWParserState();
  ~ABWParserState();
  std::map<int, int> m_tableSizes;
  std::map<std::string, ABWData> m_data;
  std::map<int, std::shared_ptr<ABWListElement>> m_listElements;

  bool m_inMetadata;
  std::string m_currentMetadataKey;
  bool m_inStyleParsing;
  std::stack<std::unique_ptr<ABWCollector> > m_collectorStack;
};

ABWParserState::ABWParserState()
  : m_tableSizes()
  , m_data()
  , m_listElements()
  , m_inMetadata(false)
  , m_currentMetadataKey()
  , m_inStyleParsing(false)
  , m_collectorStack()
{
}

ABWParserState::~ABWParserState()
{
}
} // namespace libabw

libabw::ABWParser::ABWParser(librevenge::RVNGInputStream *input, librevenge::RVNGTextInterface *iface)
  : m_input(input), m_iface(iface), m_collector(), m_state(new ABWParserState())
{
}

libabw::ABWParser::~ABWParser()
{
}

bool libabw::ABWParser::parse()
{
  if (!m_input)
    return false;

  try
  {
    m_collector.reset(new ABWStylesCollector(m_state->m_tableSizes, m_state->m_data, m_state->m_listElements));
    m_input->seek(0, librevenge::RVNG_SEEK_SET);
    m_state->m_inStyleParsing=true;
    if (!processXmlDocument(m_input))
      return false;
    updateListElementIds(m_state->m_listElements);
    m_collector.reset(new ABWContentCollector(m_iface, m_state->m_tableSizes, m_state->m_data, m_state->m_listElements));
    m_input->seek(0, librevenge::RVNG_SEEK_SET);
    m_state->m_inStyleParsing=false;
    return processXmlDocument(m_input) && m_state->m_collectorStack.empty();
  }
  catch (...)
  {
  }
  return false;
}

bool libabw::ABWParser::processXmlDocument(librevenge::RVNGInputStream *input)
{
  if (!input)
    return false;

  ABWXMLProgressWatcher watcher;
  auto reader(xmlReaderForStream(input, &watcher));
  if (!reader)
    return false;
  int ret = xmlTextReaderRead(reader.get());
  while (1 == ret && !watcher.isStuck())
  {
    ret = processXmlNode(reader.get());
    if (ret == 1)
      ret = xmlTextReaderRead(reader.get());
  }

  if (m_collector)
    m_collector->endDocument();
  return ret == 0 && !watcher.isStuck();
}

int libabw::ABWParser::processXmlNode(xmlTextReaderPtr reader)
{
  if (!reader)
    return -1;
  int tokenId = getElementToken(reader);
  int tokenType = xmlTextReaderNodeType(reader);
  int emptyToken = xmlTextReaderIsEmptyElement(reader);
  if (XML_READER_TYPE_SIGNIFICANT_WHITESPACE == tokenType)
  {
    const auto *text = (const char *)xmlTextReaderConstValue(reader);
    if (!m_state->m_inMetadata && text && text[0]==' ' && text[1]==0)
      m_collector->insertText(text);
    return 1;
  }
  else if (XML_READER_TYPE_TEXT == tokenType)
  {
    const auto *text = (const char *)xmlTextReaderConstValue(reader);
    ABW_DEBUG_MSG(("ABWParser::processXmlNode: text %s\n", text));
    if (m_state->m_inMetadata)
    {
      if (m_state->m_currentMetadataKey.empty())
      {
        ABW_DEBUG_MSG(("there is no key for metadata entry '%s'\n", text));
      }
      else
      {
        m_collector->addMetadataEntry(m_state->m_currentMetadataKey.c_str(), text);
        m_state->m_currentMetadataKey.clear();
      }
    }
    else
    {
      m_collector->insertText(text);
    }
  }

  int ret = 1;

  switch (tokenId)
  {
  case XML_ABIWORD:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readAbiword(reader);
    break;
  case XML_METADATA:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      m_state->m_inMetadata = true;
    if ((XML_READER_TYPE_END_ELEMENT == tokenType) || (emptyToken > 0))
      m_state->m_inMetadata = false;
    break;
  case XML_M:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readM(reader);
    break;
  case XML_HISTORY:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      ret = readHistory(reader);
    break;
  case XML_REVISIONS:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      ret = readRevisions(reader);
    break;
  case XML_IGNOREDWORDS:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      ret = readIgnoredWords(reader);
    break;
  case XML_S:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readS(reader);
    break;
  case XML_L:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readL(reader);
    break;
  case XML_PAGESIZE:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readPageSize(reader);
    break;
  case XML_SECTION:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readSection(reader);
    if (XML_READER_TYPE_END_ELEMENT == tokenType || emptyToken > 0)
      if (m_collector)
        m_collector->endSection();
    break;
  case XML_D:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      ret = readD(reader);
    break;
  case XML_P:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readP(reader);
    if (XML_READER_TYPE_END_ELEMENT == tokenType || emptyToken > 0)
      if (m_collector)
        m_collector->closeParagraphOrListElement();
    break;
  case XML_C:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readC(reader);
    if (XML_READER_TYPE_END_ELEMENT == tokenType || emptyToken > 0)
      if (m_collector)
        m_collector->closeSpan();
    break;
  case XML_CBR:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      m_collector->insertColumnBreak();
    break;
  case XML_PBR:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      m_collector->insertPageBreak();
    break;
  case XML_BR:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      m_collector->insertLineBreak();
    break;
  case XML_A:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readA(reader);
    if (XML_READER_TYPE_END_ELEMENT == tokenType || emptyToken > 0)
      m_collector->closeLink();
    break;
  case XML_FOOT:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readFoot(reader);
    if (XML_READER_TYPE_END_ELEMENT == tokenType || emptyToken > 0)
      m_collector->closeFoot();
    break;
  case XML_ENDNOTE:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readEndnote(reader);
    if (XML_READER_TYPE_END_ELEMENT == tokenType || emptyToken > 0)
      m_collector->closeEndnote();
    break;
  case XML_FIELD:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readField(reader);
    if (XML_READER_TYPE_END_ELEMENT == tokenType || emptyToken > 0)
      m_collector->closeField();
    break;
  case XML_TABLE:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readTable(reader);
    if (XML_READER_TYPE_END_ELEMENT == tokenType || emptyToken > 0)
      m_collector->closeTable();
    break;
  case XML_CELL:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readCell(reader);
    if (XML_READER_TYPE_END_ELEMENT == tokenType || emptyToken > 0)
      m_collector->closeCell();
    break;
  case XML_IMAGE:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readImage(reader);
    break;
  case XML_FRAME:
    if (XML_READER_TYPE_ELEMENT == tokenType)
      readFrame(reader);
    if (XML_READER_TYPE_END_ELEMENT == tokenType || emptyToken > 0)
      readCloseFrame();
    break;
  default:
    break;
  }

#ifdef DEBUG
  const xmlChar *name = xmlTextReaderConstName(reader);
  const xmlChar *value = xmlTextReaderConstValue(reader);
  int isEmptyElement = xmlTextReaderIsEmptyElement(reader);

  ABW_DEBUG_MSG(("%i %i %s", isEmptyElement, tokenType, name ? (const char *)name : ""));
  if (xmlTextReaderNodeType(reader) == 1)
  {
    while (xmlTextReaderMoveToNextAttribute(reader))
    {
      const xmlChar *name1 = xmlTextReaderConstName(reader);
      const xmlChar *value1 = xmlTextReaderConstValue(reader);
      ABW_DEBUG_MSG((" %s=\"%s\"", name1, value1));
    }
  }

  if (!value)
    ABW_DEBUG_MSG(("\n"));
  else
  {
    ABW_DEBUG_MSG((" %s\n", value));
  }
#endif

  return ret;
}

int libabw::ABWParser::getElementToken(xmlTextReaderPtr reader)
{
  return ABWXMLTokenMap::getTokenId(xmlTextReaderConstName(reader));
}

void libabw::ABWParser::readAbiword(xmlTextReaderPtr reader)
{
  const ABWXMLString props = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("props"));
  if (m_collector)
    m_collector->collectDocumentProperties(static_cast<const char *>(props));
}

void libabw::ABWParser::readM(xmlTextReaderPtr reader)
{
  const ABWXMLString key = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("key"));
  if (key)
    m_state->m_currentMetadataKey = static_cast<const char *>(key);
}

int libabw::ABWParser::readHistory(xmlTextReaderPtr reader)
{
  int ret = 1;
  int tokenId = XML_TOKEN_INVALID;
  int tokenType = -1;
  do
  {
    ret = xmlTextReaderRead(reader);
    tokenId = getElementToken(reader);
    if (XML_TOKEN_INVALID == tokenId)
    {
      ABW_DEBUG_MSG(("ABWParser::readHistory: unknown token %s\n", xmlTextReaderConstName(reader)));
    }
    tokenType = xmlTextReaderNodeType(reader);
    switch (tokenId)
    {
    default:
      break;
    }
  }
  while ((XML_HISTORY != tokenId || XML_READER_TYPE_END_ELEMENT != tokenType) && 1 == ret);
  return ret;
}

int libabw::ABWParser::readRevisions(xmlTextReaderPtr reader)
{
  int ret = 1;
  int tokenId = XML_TOKEN_INVALID;
  int tokenType = -1;
  do
  {
    ret = xmlTextReaderRead(reader);
    tokenId = getElementToken(reader);
    if (XML_TOKEN_INVALID == tokenId)
    {
      ABW_DEBUG_MSG(("ABWParser::readRevisions: unknown token %s\n", xmlTextReaderConstName(reader)));
    }
    tokenType = xmlTextReaderNodeType(reader);
    (void)tokenType;
    switch (tokenId)
    {
    default:
      break;
    }
  }
  while ((XML_REVISIONS != tokenId || XML_READER_TYPE_END_ELEMENT != tokenType) && 1 == ret);
  return ret;
}

int libabw::ABWParser::readIgnoredWords(xmlTextReaderPtr reader)
{
  int ret = 1;
  int tokenId = XML_TOKEN_INVALID;
  int tokenType = -1;
  do
  {
    ret = xmlTextReaderRead(reader);
    tokenId = getElementToken(reader);
    if (XML_TOKEN_INVALID == tokenId)
    {
      ABW_DEBUG_MSG(("ABWParser::readIgnoreWords: unknown token %s\n", xmlTextReaderConstName(reader)));
    }
    tokenType = xmlTextReaderNodeType(reader);
    switch (tokenId)
    {
    default:
      break;
    }
  }
  while ((XML_IGNOREDWORDS != tokenId || XML_READER_TYPE_END_ELEMENT != tokenType) && 1 == ret);
  return ret;
}

void libabw::ABWParser::readPageSize(xmlTextReaderPtr reader)
{
  ABWXMLString width = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("width"));
  ABWXMLString height = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("height"));
  ABWXMLString units = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("units"));
  ABWXMLString pageScale = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("page-scale"));
  if (m_collector)
    m_collector->collectPageSize((const char *)width, (const char *)height, (const char *)units, (const char *)pageScale);
}

void libabw::ABWParser::readSection(xmlTextReaderPtr reader)
{
  ABWXMLString id = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("id"));
  ABWXMLString type = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("type"));
  ABWXMLString footer = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("footer"));
  ABWXMLString footerLeft = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("footer-even"));
  ABWXMLString footerFirst = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("footer-first"));
  ABWXMLString footerLast = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("footer-last"));
  ABWXMLString header = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("header"));
  ABWXMLString headerLeft = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("header-even"));
  ABWXMLString headerFirst = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("header-first"));
  ABWXMLString headerLast = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("header-last"));
  ABWXMLString props = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("props"));

  if (!type || (xmlStrncmp(type.get(), call_BAD_CAST_OnConst("header"), 6) && xmlStrncmp(type.get(), call_BAD_CAST_OnConst("footer"), 6)))
  {
    if (m_collector)
      m_collector->collectSectionProperties((const char *)footer, (const char *)footerLeft,
                                            (const char *)footerFirst, (const char *)footerLast,
                                            (const char *)header, (const char *)headerLeft,
                                            (const char *)headerFirst, (const char *)headerLast,
                                            (const char *)props);
  }
  else
  {
    if (m_collector)
      m_collector->collectHeaderFooter((const char *)id, (const char *)type);
  }
}

int libabw::ABWParser::readD(xmlTextReaderPtr reader)
{
  ABWXMLString name = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("name"));
  ABWXMLString mimeType = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("mime-type"));

  ABWXMLString tmpBase64 = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("base64"));
  bool base64(false);
  if (tmpBase64)
  {
    findBool((const char *)tmpBase64, base64);
  }

  int ret = 1;
  int tokenId = XML_TOKEN_INVALID;
  int tokenType = -1;
  do
  {
    ret = xmlTextReaderRead(reader);
    tokenId = getElementToken(reader);
    if (XML_TOKEN_INVALID == tokenId)
    {
      ABW_DEBUG_MSG(("ABWParser::readD: unknown token %s\n", xmlTextReaderConstName(reader)));
    }
    tokenType = xmlTextReaderNodeType(reader);
    switch (tokenType)
    {
    case XML_READER_TYPE_TEXT:
    case XML_READER_TYPE_CDATA:
    {
      const xmlChar *data = xmlTextReaderConstValue(reader);
      if (data)
      {
        librevenge::RVNGBinaryData binaryData;
        if (base64)
          binaryData.appendBase64Data((const char *)data);
        else
          binaryData.append(data, (unsigned long) xmlStrlen(data));
        if (m_collector)
          m_collector->collectData((const char *)name, (const char *)mimeType, binaryData);
      }
      break;
    }
    default:
      break;
    }
  }
  while ((XML_D != tokenId || XML_READER_TYPE_END_ELEMENT != tokenType) && 1 == ret);
  return ret;
}

void libabw::ABWParser::readS(xmlTextReaderPtr reader)
{
  ABWXMLString type = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("type"));
  ABWXMLString name = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("name"));
  ABWXMLString basedon = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("basedon"));
  ABWXMLString followedby = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("followedby"));
  ABWXMLString props = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("props"));
  if (type)
  {
    if (m_collector)
    {
      switch (type[0])
      {
      case 'P':
      case 'C':
        m_collector->collectTextStyle((const char *)name, (const char *)basedon, (const char *)followedby, (const char *)props);
        break;
      default:
        break;
      }
    }
  }
}

void libabw::ABWParser::readA(xmlTextReaderPtr reader)
{
  ABWXMLString href = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("xlink:href"));
  if (m_collector)
    m_collector->openLink((const char *)href);
}

void libabw::ABWParser::readP(xmlTextReaderPtr reader)
{
  ABWXMLString level = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("level"));
  ABWXMLString listid = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("listid"));
  ABWXMLString parentid = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("listid"));
  ABWXMLString style = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("style"));
  ABWXMLString props = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("props"));
  if (m_collector)
    m_collector->collectParagraphProperties((const char *)level, (const char *)listid, (const char *)parentid,
                                            (const char *)style, (const char *)props);
}

void libabw::ABWParser::readC(xmlTextReaderPtr reader)
{
  ABWXMLString style = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("style"));
  ABWXMLString props = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("props"));
  if (m_collector)
    m_collector->collectCharacterProperties((const char *)style, (const char *)props);

}

void libabw::ABWParser::readEndnote(xmlTextReaderPtr reader)
{
  ABWXMLString id = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("endnote-id"));
  if (m_collector)
    m_collector->openEndnote((const char *)id);
}

void libabw::ABWParser::readField(xmlTextReaderPtr reader)
{
  ABWXMLString type = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("type"));
  //ABWXMLString style = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("style"));
  //ABWXMLString props = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("props"));
  ABWXMLString id = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("xid"));
  if (m_collector)
    m_collector->openField(type, id);
}

void libabw::ABWParser::readFoot(xmlTextReaderPtr reader)
{
  ABWXMLString id = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("footnote-id"));
  if (m_collector)
    m_collector->openFoot((const char *)id);
}

void libabw::ABWParser::readTable(xmlTextReaderPtr reader)
{
  ABWXMLString props = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("props"));
  if (m_collector)
    m_collector->openTable((const char *)props);
}

void libabw::ABWParser::readCell(xmlTextReaderPtr reader)
{
  ABWXMLString props = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("props"));
  if (m_collector)
    m_collector->openCell((const char *)props);
}

void libabw::ABWParser::readImage(xmlTextReaderPtr reader)
{
  ABWXMLString props = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("props"));
  ABWXMLString dataid = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("dataid"));
  if (m_collector)
    m_collector->insertImage((const char *)dataid, (const char *)props);
}

void libabw::ABWParser::readFrame(xmlTextReaderPtr reader)
{
  if (!m_collector)
    return;
  ABWXMLString props = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("props"));
  ABWXMLString imageId = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("strux-image-dataid"));
  ABWXMLString title = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("title"));
  ABWXMLString alt = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("alt"));
  if (!m_state->m_inStyleParsing)
  {
    m_state->m_collectorStack.push(std::move(m_collector));
    m_collector.reset(new ABWContentCollector(m_iface, m_state->m_tableSizes, m_state->m_data, m_state->m_listElements));
  }
  m_collector->openFrame((const char *)props, (const char *) imageId, (const char *) title, (const char *) alt);
}

void libabw::ABWParser::readCloseFrame()
{
  if (!m_collector)
    return;
  ABWOutputElements *elements=nullptr;
  bool pageFrame=false;
  m_collector->closeFrame(elements,pageFrame);
  if (m_state->m_inStyleParsing)
    return;
  if (m_state->m_collectorStack.empty())
  {
    ABW_DEBUG_MSG(("libabw::ABWParser::readCloseFrame: oops, the collector stack is empty\n"));
    return; // throw ?
  }
  if (elements)
    m_state->m_collectorStack.top()->addFrameElements(*elements, pageFrame);
  m_collector.swap(m_state->m_collectorStack.top());
  m_state->m_collectorStack.pop();
}

void libabw::ABWParser::readL(xmlTextReaderPtr reader)
{
  ABWXMLString id = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("id"));
  ABWXMLString listDecimal = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("list-decimal"));
  if (!listDecimal)
    listDecimal = xmlCharStrdup("NULL");
  ABWXMLString listDelim = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("list-delim"));
  ABWXMLString parentid = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("parentid"));
  ABWXMLString startValue = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("start-value"));
  ABWXMLString type = xmlTextReaderGetAttribute(reader, call_BAD_CAST_OnConst("type"));
  if (m_collector)
    m_collector->collectList((const char *)id, (const char *)listDecimal, (const char *)listDelim,
                             (const char *)parentid, (const char *)startValue, (const char *)type);
}

/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
