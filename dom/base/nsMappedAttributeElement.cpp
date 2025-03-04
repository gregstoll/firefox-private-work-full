/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsMappedAttributeElement.h"
#include "nsMappedAttributes.h"
#include "mozilla/dom/Document.h"

bool nsMappedAttributeElement::SetAndSwapMappedAttribute(nsAtom* aName,
                                                         nsAttrValue& aValue,
                                                         bool* aValueWasSet,
                                                         nsresult* aRetval) {
  *aRetval = mAttrs.SetAndSwapMappedAttr(aName, aValue, this, aValueWasSet);
  return true;
}

nsMapRuleToAttributesFunc
nsMappedAttributeElement::GetAttributeMappingFunction() const {
  return &MapNoAttributesInto;
}

void nsMappedAttributeElement::MapNoAttributesInto(
    const nsMappedAttributes*, mozilla::MappedDeclarations&) {}

void nsMappedAttributeElement::NodeInfoChanged(Document* aOldDoc) {
  if (mAttrs.HasMappedAttrs()) {
    mAttrs.SetMappedAttributeStyles(OwnerDoc()->GetAttributeStyles());
  }
  nsMappedAttributeElementBase::NodeInfoChanged(aOldDoc);
}
