/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 * This file is part of the libabw project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef __ABWPARSER_H__
#define __ABWPARSER_H__

#include <memory>

#include <librevenge/librevenge.h>
#include "ABWXMLHelper.h"

namespace libabw
{

class ABWCollector;
struct ABWParserState;

class ABWParser
{
public:
  explicit ABWParser(librevenge::RVNGInputStream *input, librevenge::RVNGTextInterface *iface);
  virtual ~ABWParser();
  bool parse();

private:
  ABWParser();
  ABWParser(const ABWParser &);
  ABWParser &operator=(const ABWParser &);

  // Helper functions

  int getElementToken(xmlTextReaderPtr reader);

  // Functions to read the AWML document structure

  bool processXmlDocument(librevenge::RVNGInputStream *input);
  int processXmlNode(xmlTextReaderPtr reader);

  void readAbiword(xmlTextReaderPtr reader);
  void readM(xmlTextReaderPtr reader);
  int readHistory(xmlTextReaderPtr reader);
  int readRevisions(xmlTextReaderPtr reader);
  int readIgnoredWords(xmlTextReaderPtr reader);
  void readPageSize(xmlTextReaderPtr reader);
  void readSection(xmlTextReaderPtr reader);
  void readA(xmlTextReaderPtr reader);
  void readC(xmlTextReaderPtr reader);
  int readD(xmlTextReaderPtr reader);
  void readL(xmlTextReaderPtr reader);
  void readP(xmlTextReaderPtr reader);
  void readS(xmlTextReaderPtr reader);
  void readEndnote(xmlTextReaderPtr reader);
  void readFoot(xmlTextReaderPtr reader);
  void readField(xmlTextReaderPtr reader);
  void readImage(xmlTextReaderPtr reader);

  void readTable(xmlTextReaderPtr reader);
  void readCell(xmlTextReaderPtr reader);

  void readFrame(xmlTextReaderPtr reader);
  void readCloseFrame();

  librevenge::RVNGInputStream *m_input;
  librevenge::RVNGTextInterface *m_iface;
  std::unique_ptr<ABWCollector> m_collector;
  std::unique_ptr<ABWParserState> m_state;
};

} // namespace libabw

#endif // __ABWPARSER_H__
/* vim:set shiftwidth=2 softtabstop=2 expandtab: */
