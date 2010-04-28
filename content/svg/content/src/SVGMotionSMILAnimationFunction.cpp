/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Mozilla SVG project.
 *
 * The Initial Developer of the Original Code is the Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Daniel Holbert <dholbert@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "SVGMotionSMILAnimationFunction.h"
#include "nsSMILParserUtils.h"
#include "nsSVGAngle.h"
#include "SVGMotionSMILType.h"
#include "SVGMotionSMILPathUtils.h"

namespace mozilla {

SVGMotionSMILAnimationFunction::SVGMotionSMILAnimationFunction()
  : mRotateType(eRotateType_Explicit),
    mRotateAngle(0.0f),
    mPathSourceType(ePathSourceType_None),
    mIsPathStale(PR_TRUE)  // Try to initialize path on first GetValues call
{
}

void
SVGMotionSMILAnimationFunction::MarkStaleIfAttributeAffectsPath(nsIAtom* aAttribute)
{
  PRBool isAffected;
  if (aAttribute == nsGkAtoms::path) {
    isAffected = (mPathSourceType <= ePathSourceType_PathAttr);
  } else if (aAttribute == nsGkAtoms::values) {
    isAffected = (mPathSourceType <= ePathSourceType_ValuesAttr);
  } else if (aAttribute == nsGkAtoms::from ||
             aAttribute == nsGkAtoms::to) {
    isAffected = (mPathSourceType <= ePathSourceType_ToAttr);
  } else if (aAttribute == nsGkAtoms::by) {
    isAffected = (mPathSourceType <= ePathSourceType_ByAttr);
  } else {
    NS_NOTREACHED("Should only call this method for path-describing attrs");
    isAffected = PR_FALSE;
  }

  if (isAffected) {
    mIsPathStale = PR_TRUE;
    mHasChanged = PR_TRUE;
  }
}

PRBool
SVGMotionSMILAnimationFunction::SetAttr(nsIAtom* aAttribute,
                                        const nsAString& aValue,
                                        nsAttrValue& aResult,
                                        nsresult* aParseResult)
{
  if (aAttribute == nsGkAtoms::rotate) {
    nsresult rv = SetRotate(aValue, aResult);
    if (aParseResult) {
      *aParseResult = rv;
    }
  } else if (aAttribute == nsGkAtoms::by ||
             aAttribute == nsGkAtoms::from ||
             aAttribute == nsGkAtoms::to ||
             aAttribute == nsGkAtoms::values) {
    MarkStaleIfAttributeAffectsPath(aAttribute);
  } else {
    // Defer to superclass method
    return nsSMILAnimationFunction::SetAttr(aAttribute, aValue,
                                            aResult, aParseResult);
  }

  return PR_TRUE;
}

PRBool
SVGMotionSMILAnimationFunction::UnsetAttr(nsIAtom* aAttribute)
{
  if (aAttribute == nsGkAtoms::rotate) {
    UnsetRotate();
  } else if (aAttribute == nsGkAtoms::by ||
             aAttribute == nsGkAtoms::from ||
             aAttribute == nsGkAtoms::to ||
             aAttribute == nsGkAtoms::values) {
    MarkStaleIfAttributeAffectsPath(aAttribute);
  } else {
    // Defer to superclass method
    return nsSMILAnimationFunction::UnsetAttr(aAttribute);
  }

  return PR_TRUE;
}

nsSMILAnimationFunction::nsSMILCalcMode
SVGMotionSMILAnimationFunction::GetCalcMode() const
{
  const nsAttrValue* value = GetAttr(nsGkAtoms::calcMode);
  if (!value) {
    return CALC_PACED; // animateMotion defaults to calcMode="paced"
  }

  return nsSMILCalcMode(value->GetEnumValue());
}

//----------------------------------------------------------------------
// Helpers for GetValues

void
SVGMotionSMILAnimationFunction::
  RebuildPathAndVerticesFromBasicAttrs(const nsIContent* aContextElem)
{
  NS_ABORT_IF_FALSE(!mPath, "regenerating when we aleady have path");
  NS_ABORT_IF_FALSE(mPathVertices.IsEmpty(),
                    "regenerating when we already have vertices");

  if (aContextElem->GetNameSpaceID() != kNameSpaceID_SVG) {
    NS_ERROR("Uh oh, SVG animateMotion element targeting a non-SVG node");
    return;
  }

  // NOTE: We have to cast away constness on context node, since the
  // nsSVGLength2 methods that need it for unit-conversion
  // (e.g. nsSVGLength2::GetBaseValue) aren't const-correct.
  nsSVGElement* svgCtx =
    static_cast<nsSVGElement*>(const_cast<nsIContent*>(aContextElem));
  SVGMotionSMILPathUtils::PathGenerator pathGenerator(svgCtx);

  PRBool success = PR_FALSE;
  if (HasAttr(nsGkAtoms::values)) {
    // Generate path based on our values array
    mPathSourceType = ePathSourceType_ValuesAttr;
    const nsAString& valuesStr = GetAttr(nsGkAtoms::values)->GetStringValue();
    SVGMotionSMILPathUtils::MotionValueParser parser(&pathGenerator,
                                                     &mPathVertices);
    success =
      NS_SUCCEEDED(nsSMILParserUtils::ParseValuesGeneric(valuesStr, parser));
  } else if (HasAttr(nsGkAtoms::to) || HasAttr(nsGkAtoms::by)) {
    // Apply 'from' value (or a dummy 0,0 'from' value)
    if (HasAttr(nsGkAtoms::from)) {
      const nsAString& fromStr = GetAttr(nsGkAtoms::from)->GetStringValue();
      success = pathGenerator.MoveToAbsolute(fromStr);
      mPathVertices.AppendElement(0.0);
    } else {
      // Create dummy 'from' value at 0,0, if we're doing by-animation.
      // (NOTE: We don't add the dummy 0-point to our list for *to-animation*,
      // because the nsSMILAnimationFunction logic for to-animation doesn't
      // expect a dummy value. It only expects one value: the final 'to' value.)
      pathGenerator.MoveToOrigin();
      if (!HasAttr(nsGkAtoms::to)) {
        mPathVertices.AppendElement(0.0);
      }
      success = PR_TRUE;
    }

    // Apply 'to' or 'by' value
    if (success) {
      double dist;
      if (HasAttr(nsGkAtoms::to)) {
        mPathSourceType = ePathSourceType_ToAttr;
        const nsAString& toStr = GetAttr(nsGkAtoms::to)->GetStringValue();
        success = pathGenerator.LineToAbsolute(toStr, dist);
      } else { // HasAttr(nsGkAtoms::by)
        mPathSourceType = ePathSourceType_ByAttr;
        const nsAString& byStr = GetAttr(nsGkAtoms::by)->GetStringValue();
        success = pathGenerator.LineToRelative(byStr, dist);
      }
      if (success) {
        mPathVertices.AppendElement(dist);
      }
    }
  }
  if (success) {
    mPath = pathGenerator.GetResultingPath();
  } else {
    // Parse failure. Leave path as null, and clear path-related member data.
    mPathVertices.Clear();
  }
}

// Helper to regenerate our path representation & its list of vertices
void
SVGMotionSMILAnimationFunction::
  RebuildPathAndVertices(const nsIContent* aTargetElement)
{
  NS_ABORT_IF_FALSE(mIsPathStale, "rebuilding path when it isn't stale");

  // Clear stale data
  mPath = nsnull;
  mPathVertices.Clear();
  mPathSourceType = ePathSourceType_None;

  // Do we have a mpath child? if so, it trumps everything. Otherwise, we look
  // through our list of path-defining attributes, in order of priority.
  // XXXdholbert This is where we should check for mpath, in <mpath> patch.
  // ...else, if no mpath child:
  // XXXdholbert This is where we should check for path attr, in |path| patch.
  // ...else {
  {
    // Get path & vertices from basic SMIL attrs: from/by/to/values

    RebuildPathAndVerticesFromBasicAttrs(aTargetElement);
    mValueNeedsReparsingEverySample = PR_TRUE;
  }
  mIsPathStale = PR_FALSE;
}

PRBool
SVGMotionSMILAnimationFunction::
  GenerateValuesForPathAndPoints(gfxFlattenedPath* aPath,
                                 nsTArray<double>& aPointDistances,
                                 nsTArray<nsSMILValue>& aResult)
{
  NS_ABORT_IF_FALSE(aResult.IsEmpty(), "outparam is non-empty");

  const PRUint32 numPoints = aPointDistances.Length();
  for (PRUint32 i = 0; i < numPoints; ++i) {
    double curDist = aPointDistances[i];
    if (!aResult.AppendElement(
          SVGMotionSMILType::ConstructSMILValue(aPath, curDist,
                                                mRotateType, mRotateAngle))) {
      return PR_FALSE;
    }
  }
  return PR_TRUE;
}

nsresult
SVGMotionSMILAnimationFunction::GetValues(const nsISMILAttr& aSMILAttr,
                                          nsSMILValueArray& aResult)
{
  if (mIsPathStale) {
    RebuildPathAndVertices(aSMILAttr.GetTargetNode());
  }
  NS_ABORT_IF_FALSE(!mIsPathStale, "Forgot to clear 'is path stale' state");

  if (!mPath) {
    // This could be due to e.g. a parse error.
    NS_ABORT_IF_FALSE(mPathVertices.IsEmpty(), "have vertices but no path");
    return NS_ERROR_FAILURE;
  }
  NS_ABORT_IF_FALSE(!mPathVertices.IsEmpty(), "have a path but no vertices");

  // Now: Make the actual list of nsSMILValues
  PRBool success = GenerateValuesForPathAndPoints(mPath, mPathVertices,
                                                  aResult);
  if (!success) {
    return NS_ERROR_OUT_OF_MEMORY;
  }

  return NS_OK;
}

nsresult
SVGMotionSMILAnimationFunction::SetRotate(const nsAString& aRotate,
                                          nsAttrValue& aResult)
{
  mHasChanged = PR_TRUE;

  aResult.SetTo(aRotate);
  if (aRotate.EqualsLiteral("auto")) {
    mRotateType = eRotateType_Auto;
  } else if (aRotate.EqualsLiteral("auto-reverse")) {
    mRotateType = eRotateType_AutoReverse;
  } else {
    mRotateType = eRotateType_Explicit;

    // Parse numeric angle string, with the help of a temp nsSVGAngle.
    nsSVGAngle svgAngle;
    svgAngle.Init();
    nsresult rv = svgAngle.SetBaseValueString(aRotate, nsnull, PR_FALSE);
    if (NS_FAILED(rv)) { // Parse error
      mRotateAngle = 0.0f; // set default rotate angle
      // XXX report to console?
      return rv;
    }

    mRotateAngle = svgAngle.GetBaseValInSpecifiedUnits();

    // Convert to radian units, if we're not already in radians.
    PRUint8 angleUnit = svgAngle.GetBaseValueUnit();
    if (angleUnit != nsIDOMSVGAngle::SVG_ANGLETYPE_RAD) {
      mRotateAngle *= nsSVGAngle::GetDegreesPerUnit(angleUnit) /
        nsSVGAngle::GetDegreesPerUnit(nsIDOMSVGAngle::SVG_ANGLETYPE_RAD);
    }
  }
  return NS_OK;
}

void
SVGMotionSMILAnimationFunction::UnsetRotate()
{
  mRotateAngle = 0.0f; // default value
  mRotateType = eRotateType_Explicit;
  mHasChanged = PR_TRUE;
}

PRBool
SVGMotionSMILAnimationFunction::TreatSingleValueAsStatic() const
{
  // <animateMotion> has two more ways that we could be just sampling a single
  // value -- via path attribute and the <mpath> element, with a path
  // description that just includes a single "move" command.
  return (mPathSourceType == ePathSourceType_ValuesAttr ||
          mPathSourceType == ePathSourceType_PathAttr ||
          mPathSourceType == ePathSourceType_Mpath);
}

} // namespace mozilla
