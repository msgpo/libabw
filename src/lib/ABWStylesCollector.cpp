/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * This file is part of the libabw project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <boost/spirit/include/classic.hpp>
#include <boost/algorithm/string.hpp>
#include <librevenge/librevenge.h>
#include "ABWStylesCollector.h"
#include "libabw_internal.h"

#define ABW_EPSILON 1.0E-06

namespace libabw
{

namespace
{

enum ABWUnit
{
  ABW_NONE,
  ABW_CM,
  ABW_IN,
  ABW_MM,
  ABW_PI,
  ABW_PT,
  ABW_PX,
  ABW_PERCENT
};

enum ABWListType
{
  NUMBERED_LIST = 0,
  LOWERCASE_LIST = 1,
  UPPERCASE_LIST = 2,
  LOWERROMAN_LIST = 3,
  UPPERROMAN_LIST = 4,

  BULLETED_LIST = 5,
  DASHED_LIST = 6,
  SQUARE_LIST = 7,
  TRIANGLE_LIST = 8,
  DIAMOND_LIST = 9,
  STAR_LIST = 10,
  IMPLIES_LIST = 11,
  TICK_LIST = 12,
  BOX_LIST = 13,
  HAND_LIST = 14,
  HEART_LIST = 15,
  ARROWHEAD_LIST = 16,

  LAST_BULLETED_LIST = 17,
  OTHER_NUMBERED_LISTS = 0x7f,
  ARABICNUMBERED_LIST = 0x80,
  HEBREW_LIST = 0x81,
  NOT_A_LIST = 0xff
};

static bool findInt(const std::string &str, int &res)
{
  using namespace ::boost::spirit::classic;

  if (str.empty())
    return false;

  return parse(str.c_str(),
               //  Begin grammar
               (
                 int_p[assign_a(res)]
               ) >> end_p,
               //  End grammar
               space_p).full;
}

static void parsePropString(const std::string &str, std::map<std::string, std::string> &props)
{
  if (str.empty())
    return;

  std::string propString = boost::trim_copy(str);
  std::vector<std::string> strVec;
  boost::algorithm::split(strVec, propString, boost::is_any_of(";"), boost::token_compress_on);
  for (std::vector<std::string>::size_type i = 0; i < strVec.size(); ++i)
  {
    boost::algorithm::trim(strVec[i]);
    std::vector<std::string> tmpVec;
    boost::algorithm::split(tmpVec, strVec[i], boost::is_any_of(":"), boost::token_compress_on);
    if (tmpVec.size() == 2)
      props[tmpVec[0]] = tmpVec[1];
  }
}

} // anonymous namespace

} // namespace libabw

libabw::ABWStylesTableState::ABWStylesTableState() :
  m_currentCellProperties(),
  m_currentTableWidth(0),
  m_currentTableRow(-1),
  m_currentTableId(-1) {}

libabw::ABWStylesTableState::ABWStylesTableState(const ABWStylesTableState &ts) :
  m_currentCellProperties(ts.m_currentCellProperties),
  m_currentTableWidth(ts.m_currentTableWidth),
  m_currentTableRow(ts.m_currentTableRow),
  m_currentTableId(ts.m_currentTableId) {}

libabw::ABWStylesTableState::~ABWStylesTableState() {}

libabw::ABWStylesParsingState::ABWStylesParsingState() :
  m_tableStates() {}

libabw::ABWStylesParsingState::ABWStylesParsingState(const ABWStylesParsingState &ps) :
  m_tableStates(ps.m_tableStates) {}

libabw::ABWStylesParsingState::~ABWStylesParsingState() {}

libabw::ABWStylesCollector::ABWStylesCollector(std::map<int, int> &tableSizes,
                                               std::map<std::string, ABWData> &data,
                                               std::map<librevenge::RVNGString, ABWListElement *> &listElements) :
  m_ps(new ABWStylesParsingState),
  m_tableSizes(tableSizes),
  m_data(data),
  m_tableCounter(0),
  m_listElements(listElements) {}

libabw::ABWStylesCollector::~ABWStylesCollector()
{
  DELETEP(m_ps);
}

void libabw::ABWStylesCollector::openTable(const char *)
{
  m_ps->m_tableStates.push(ABWStylesTableState());
  m_ps->m_tableStates.top().m_currentTableId = m_tableCounter++;
  m_ps->m_tableStates.top().m_currentTableRow = -1;
  m_ps->m_tableStates.top().m_currentTableWidth = 0;
}

void libabw::ABWStylesCollector::closeTable()
{
  m_tableSizes[m_ps->m_tableStates.top().m_currentTableId]
    = m_ps->m_tableStates.top().m_currentTableWidth;
  if (!m_ps->m_tableStates.empty())
    m_ps->m_tableStates.pop();
}

void libabw::ABWStylesCollector::openCell(const char *props)
{
  if (props)
    parsePropString(props, m_ps->m_tableStates.top().m_currentCellProperties);
  int currentRow(0);
  if (!findInt(_findCellProperty("top-attach"), currentRow))
    currentRow = m_ps->m_tableStates.top().m_currentTableRow + 1;
  while (m_ps->m_tableStates.top().m_currentTableRow < currentRow)
    m_ps->m_tableStates.top().m_currentTableRow++;

  if (!m_ps->m_tableStates.empty() && 0 == m_ps->m_tableStates.top().m_currentTableRow)
  {
    int leftAttach(0);
    int rightAttach(0);
    if (findInt(_findCellProperty("left-attach"), leftAttach) && findInt(_findCellProperty("right-attach"), rightAttach))
      m_ps->m_tableStates.top().m_currentTableWidth += rightAttach - leftAttach;
    else
      m_ps->m_tableStates.top().m_currentTableWidth++;
  }
}

void libabw::ABWStylesCollector::closeCell()
{
  m_ps->m_tableStates.top().m_currentCellProperties.clear();
}

std::string libabw::ABWStylesCollector::_findCellProperty(const char *name)
{
  std::map<std::string, std::string>::const_iterator iter = m_ps->m_tableStates.top().m_currentCellProperties.find(name);
  if (iter != m_ps->m_tableStates.top().m_currentCellProperties.end())
    return iter->second;
  return std::string();
}

void libabw::ABWStylesCollector::collectData(const char *name, const char *mimeType, const librevenge::RVNGBinaryData &data)
{
  if (!name)
    return;
  m_data[name] = ABWData(mimeType, data);
}


void libabw::ABWStylesCollector::collectList(const char *id, const char *, const char *listDelim,
                                             const char *, const char *startValue, const char *type)
{
  using namespace boost;
  using namespace boost::algorithm;

  if (!id)
    return;
  if (m_listElements[id])
    delete m_listElements[id];
  int intType;
  if (!type || !findInt(type, intType))
    intType = 5;
  if (intType >= BULLETED_LIST && intType < LAST_BULLETED_LIST)
  {
    ABWUnorderedListElement *tmpElement = new ABWUnorderedListElement();
    switch (intType)
    {
    case BULLETED_LIST:
    case DASHED_LIST:
    case SQUARE_LIST:
    case TRIANGLE_LIST:
    case DIAMOND_LIST:
    case STAR_LIST:
    case IMPLIES_LIST:
    case TICK_LIST:
    case BOX_LIST:
    case HAND_LIST:
    case HEART_LIST:
    case ARROWHEAD_LIST:
    default:
      tmpElement->m_bulletChar = "*"; // for the while
      break;
    }
    m_listElements[id] = tmpElement;
  }
  else
  {
    ABWOrderedListElement *tmpElement = new ABWOrderedListElement();
    switch (intType)
    {
    case NUMBERED_LIST:
      tmpElement->m_numFormat = "1";
      break;
    case LOWERCASE_LIST:
      tmpElement->m_numFormat = "a";
      break;
    case UPPERCASE_LIST:
      tmpElement->m_numFormat = "A";
      break;
    case LOWERROMAN_LIST:
      tmpElement->m_numFormat = "i";
      break;
    case UPPERROMAN_LIST:
      tmpElement->m_numFormat = "I";
      break;
    default:
      tmpElement->m_numFormat = "1";
      break;
    }
    if (!startValue || !findInt(startValue, tmpElement->m_startValue))
      tmpElement->m_startValue = 0;

    // get prefix and suffix by splitting the listDelim
    if (listDelim)
    {
      std::string delim(listDelim);
      std::vector<librevenge::RVNGString> strVec;

      for (split_iterator<std::string::iterator> It =
             make_split_iterator(delim, first_finder("%L", is_iequal()));
           It != split_iterator<std::string::iterator>(); ++It)
      {
        strVec.push_back(copy_range<std::string>(*It).c_str());
      }
      if (2 <= strVec.size())
      {
        tmpElement->m_numPrefix = strVec[0];
        tmpElement->m_numSuffix = strVec[1];
      }
    }
    m_listElements[id] = tmpElement;
  }
}
/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
