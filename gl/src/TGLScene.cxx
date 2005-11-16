// @(#)root/gl:$Name:  $:$Id: TGLScene.cxx,v 1.22 2005/10/24 14:49:33 brun Exp $
// Author:  Richard Maunder  25/05/2005
// Parts taken from original TGLRender by Timur Pocheptsov

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

// TODO: Function descriptions
// TODO: Class def - same as header

#include "TGLScene.h"
#include "TGLCamera.h"
#include "TGLLogicalShape.h"
#include "TGLPhysicalShape.h"
#include "TGLStopwatch.h"
#include "TGLDisplayListCache.h"
#include "TGLClip.h"
#include "TGLIncludes.h"
#include "TError.h"
#include "TString.h"
#include "Riostream.h"
#include "TClass.h" // For non-TObject reflection

#include <algorithm>

ClassImp(TGLScene)

//______________________________________________________________________________
TGLScene::TGLScene() :
   fLock(kUnlocked), fDrawList(1000), 
   fDrawListValid(kFALSE), fBoundingBox(), fBoundingBoxValid(kFALSE), 
   fLastDrawLOD(kHigh), fSelectedPhysical(0)
{
}

//______________________________________________________________________________
TGLScene::~TGLScene()
{
   TakeLock(kModifyLock);
   DestroyPhysicals(kTRUE); // including modified
   DestroyLogicals();
   ReleaseLock(kModifyLock);

   // Purge out the DL cache - when per drawable done no longer required
   TGLDisplayListCache::Instance().Purge();
}

//TODO: Inline
//______________________________________________________________________________
void TGLScene::AdoptLogical(TGLLogicalShape & shape)
{
   if (fLock != kModifyLock) {
      Error("TGLScene::AdoptLogical", "expected ModifyLock");
      return;
   }

   // TODO: Very inefficient check - disable
   assert(fLogicalShapes.find(shape.ID()) == fLogicalShapes.end());
   fLogicalShapes.insert(LogicalShapeMapValueType_t(shape.ID(), &shape));
}

//______________________________________________________________________________
Bool_t TGLScene::DestroyLogical(ULong_t ID)
{
   if (fLock != kModifyLock) {
      Error("TGLScene::DestroyLogical", "expected ModifyLock");
      return kFALSE;
   }

   LogicalShapeMapIt_t logicalIt = fLogicalShapes.find(ID);
   if (logicalIt != fLogicalShapes.end()) {
      const TGLLogicalShape * logical = logicalIt->second;
      if (logical->Ref() == 0) {
         fLogicalShapes.erase(logicalIt);
         delete logical;
         return kTRUE;
      } else {
         assert(kFALSE);
      }
   }

   return kFALSE;
}

//______________________________________________________________________________
UInt_t TGLScene::DestroyLogicals()
{
   UInt_t count = 0;
   if (fLock != kModifyLock) {
      Error("TGLScene::DestroyLogicals", "expected ModifyLock");
      return count;
   }

   LogicalShapeMapIt_t logicalShapeIt = fLogicalShapes.begin();
   const TGLLogicalShape * logicalShape;
   while (logicalShapeIt != fLogicalShapes.end()) {
      logicalShape = logicalShapeIt->second;
      if (logicalShape) {
         if (logicalShape->Ref() == 0) {
            fLogicalShapes.erase(logicalShapeIt++);
            delete logicalShape;
            ++count;
            continue;
         } else {
            assert(kFALSE);
         }
      } else {
         assert(kFALSE);
      }
      ++logicalShapeIt;
   }

   return count;
}

//TODO: Inline
//______________________________________________________________________________
TGLLogicalShape * TGLScene::FindLogical(ULong_t ID) const
{
   LogicalShapeMapCIt_t it = fLogicalShapes.find(ID);
   if (it != fLogicalShapes.end()) {
      return it->second;
   } else {
      return 0;
   }
}

//TODO: Inline
//______________________________________________________________________________
void TGLScene::AdoptPhysical(TGLPhysicalShape & shape)
{
   if (fLock != kModifyLock) {
      Error("TGLScene::AdoptPhysical", "expected ModifyLock");
      return;
   }
   // TODO: Very inefficient check - disable
   assert(fPhysicalShapes.find(shape.ID()) == fPhysicalShapes.end());

   fPhysicalShapes.insert(PhysicalShapeMapValueType_t(shape.ID(), &shape));
   fBoundingBoxValid = kFALSE;

   // Add into draw list and mark for sorting
   fDrawList.push_back(&shape);
   fDrawListValid = kFALSE;
}

//______________________________________________________________________________
Bool_t TGLScene::DestroyPhysical(ULong_t ID)
{
   if (fLock != kModifyLock) {
      Error("TGLScene::DestroyPhysical", "expected ModifyLock");
      return kFALSE;
   }
   PhysicalShapeMapIt_t physicalIt = fPhysicalShapes.find(ID);
   if (physicalIt != fPhysicalShapes.end()) {
      TGLPhysicalShape * physical = physicalIt->second;
      if (fSelectedPhysical == physical) {
         fSelectedPhysical = 0;
      }
      fPhysicalShapes.erase(physicalIt);
      fBoundingBoxValid = kFALSE;
  
      // Zero the draw list entry - will be erased as part of sorting
      DrawListIt_t drawIt = find(fDrawList.begin(), fDrawList.end(), physical);
      if (drawIt != fDrawList.end()) {
         *drawIt = 0;
         fDrawListValid = kFALSE;
      } else {
         assert(kFALSE);
      }
      delete physical;
      return kTRUE;
   }

   return kFALSE;
}

//______________________________________________________________________________
UInt_t TGLScene::DestroyPhysicals(Bool_t incModified, const TGLCamera * camera)
{
   if (fLock != kModifyLock) {
      Error("TGLScene::DestroyPhysicals", "expected ModifyLock");
      return kFALSE;
   }
   UInt_t count = 0;
   PhysicalShapeMapIt_t physicalShapeIt = fPhysicalShapes.begin();
   const TGLPhysicalShape * physical;
   while (physicalShapeIt != fPhysicalShapes.end()) {
      physical = physicalShapeIt->second;
      if (physical) {
         // Destroy any physical shape no longer of interest to camera
         // If modified options allow this physical to be destoyed
         if (incModified || (!incModified && !physical->IsModified())) {
            // and no camera is passed, or it is no longer of interest
            // to camera
            if (!camera || (camera && !camera->OfInterest(physical->BoundingBox()))) {

               // Then we can destroy it - remove from map
               fPhysicalShapes.erase(physicalShapeIt++);

               // Zero the draw list entry - will be erased as part of sorting
               DrawListIt_t drawIt = find(fDrawList.begin(), fDrawList.end(), physical);
               if (drawIt != fDrawList.end()) {
                  *drawIt = 0;
               } else {
                  assert(kFALSE);
               }

               // Ensure if selected object this is cleared
               if (fSelectedPhysical == physical) {
                  fSelectedPhysical = 0;
               }
               // Finally destroy actual object
               delete physical;
               ++count;
               continue; // Incremented the iterator during erase()
            }
         }
      } else {
         assert(kFALSE);
      }
      ++physicalShapeIt;
   }

   if (count > 0) {
      fBoundingBoxValid = kFALSE;
      fDrawListValid = kFALSE;
   }

   return count;
}

//TODO: Inline
//______________________________________________________________________________
TGLPhysicalShape * TGLScene::FindPhysical(ULong_t ID) const
{
   PhysicalShapeMapCIt_t it = fPhysicalShapes.find(ID);
   if (it != fPhysicalShapes.end()) {
      return it->second;
   } else {
      return 0;
   }
}

//______________________________________________________________________________
//TODO: Merge axes flag and LOD into general draw flag
void TGLScene::Draw(const TGLCamera & camera, EDrawStyle style, UInt_t LOD, 
                    Double_t timeout, const TGLClip * clip)
{
   if (fLock != kDrawLock && fLock != kSelectLock) {
      Error("TGLScene::Draw", "expected Draw or Select Lock");
   }

   // Reset debug draw stats
   ResetDrawStats();

   // Sort the draw list if required
   if (!fDrawListValid) {
      SortDrawList();
   }

   // Setup GL for current draw style - fill, wireframe, outline
   // Any GL modifications need to be defered until drawing time - 
   // to ensure we are in correct thread/context under Windows
   // TODO: Could detect change and only mod if changed for speed
   switch (style) {
      case (kFill): {
         glEnable(GL_LIGHTING);
         glEnable(GL_CULL_FACE);
         glPolygonMode(GL_FRONT, GL_FILL);
         glClearColor(0.0, 0.0, 0.0, 1.0); // Black
         break;
      }
      case (kWireFrame): {
         glDisable(GL_CULL_FACE);
         glDisable(GL_LIGHTING);
         glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
         glClearColor(0.0, 0.0, 0.0, 1.0); // Black
         break;
      }
      case (kOutline): {
         glEnable(GL_LIGHTING);
         glEnable(GL_CULL_FACE);
         glPolygonMode(GL_FRONT, GL_FILL);
         glClearColor(1.0, 1.0, 1.0, 1.0); // White
         break;
      }
      default: {
         assert(kFALSE);
      }
   }

   // If no clip object
   if (!clip) {
      DrawPass(camera, style, LOD, timeout);
   } else {
      // Get the clip plane set from the clipping object
      std::vector<TGLPlane> planeSet;
      clip->PlaneSet(planeSet);

      // Strip any planes that outside the scene bounding box - no effect
      for (std::vector<TGLPlane>::iterator it = planeSet.begin();
           it != planeSet.end(); ) {
         if (BoundingBox().Overlap(*it) == kOutside) {
            it = planeSet.erase(it);
         } else {
            ++it;
         }
      }

      if (gDebug>2) {
         Info("TGLScene::Draw()", "%d active clip planes", planeSet.size());
      }
      // Limit to smaller of plane set size or GL implementation plane support
      Int_t maxGLPlanes;
      glGetIntegerv(GL_MAX_CLIP_PLANES, &maxGLPlanes);
      UInt_t maxPlanes = maxGLPlanes;
      UInt_t planeInd;
      if (planeSet.size() < maxPlanes) {
         maxPlanes = planeSet.size();
      }

      // Note : OpenGL Reference (Blue Book) states
      // GL_CLIP_PLANEi = CL_CLIP_PLANE0 + i

      // Clip away scene outside of the clip object
      if (clip->Mode() == TGLClip::kOutside) {
         // Load all negated clip planes (up to max) at once
         for (UInt_t i=0; i<maxPlanes; i++) {
            planeSet[i].Negate();
            glClipPlane(GL_CLIP_PLANE0+i, planeSet[i].CArr());
            glEnable(GL_CLIP_PLANE0+i);
         }

          // Draw scene once with full time slot, passing all the planes
          DrawPass(camera, style, LOD, timeout, &planeSet);
      }
      // Clip away scene inside of the clip object
      else {
         std::vector<TGLPlane> activePlanes;
         for (planeInd=0; planeInd<maxPlanes; planeInd++) {
            if (planeInd > 0) {
               activePlanes[planeInd - 1].Negate();
               glClipPlane(GL_CLIP_PLANE0+planeInd - 1, activePlanes[planeInd - 1].CArr());
            }
            activePlanes.push_back(planeSet[planeInd]);
            glClipPlane(GL_CLIP_PLANE0+planeInd, activePlanes[planeInd].CArr());
            glEnable(GL_CLIP_PLANE0+planeInd);
            DrawPass(camera, style, LOD, timeout/maxPlanes, &activePlanes);
         }
      }
      // Ensure all clip planes turned off again
      for (planeInd=0; planeInd<maxPlanes; planeInd++) {
         glDisable(GL_CLIP_PLANE0+planeInd);
      }
   }

   // Reset style related modes set above to defaults
   glEnable(GL_LIGHTING);
   glEnable(GL_CULL_FACE);
   glPolygonMode(GL_FRONT, GL_FILL);

   // Record this so that any Select() draw can be redone at same quality and ensure
   // accuracy of picking
   fLastDrawLOD = LOD;

   // TODO: Should record if full scene can be drawn at 100% in a target time (set on scene)
   // Then timeout should not be passed - just bool if termination is permitted - which may
   // be ignored if all can be done. Pass back bool to indicate if whole scene could be drawn
   // at desired quality. Need a fixed target time so valid across multiple draws

   // Dump debug draw stats
   DumpDrawStats();

   return;
}

//______________________________________________________________________________
void TGLScene::DrawPass(const TGLCamera & camera, EDrawStyle style, UInt_t LOD, 
                        Double_t timeout, const std::vector<TGLPlane> * clipPlanes)
{
   // Set stopwatch running
   TGLStopwatch stopwatch;
   stopwatch.Start();

   // Setup draw style function pointer
   void (TGLPhysicalShape::*drawPtr)(UInt_t)const = &TGLPhysicalShape::Draw;
   if (style == kWireFrame) {
      drawPtr = &TGLPhysicalShape::DrawWireFrame;
   } else if (style == kOutline) {
      drawPtr = &TGLPhysicalShape::DrawOutline;
   }

   // Step 1: Loop through the main sorted draw list 
   Bool_t                   run = kTRUE;
   const TGLPhysicalShape * drawShape;
   Bool_t                   doSelected = (fSelectedPhysical != 0);

   // Transparent list built on fly
   static DrawList_t transDrawList;
   transDrawList.reserve(fDrawList.size() / 10); // assume less 10% of total
   transDrawList.clear();

   // Opaque only objects drawn in first loop
   // TODO: Sort front -> back for better performance
   glDepthMask(GL_TRUE);
   // If the scene bounding box is inside the camera frustum then
   // no need to check individual shapes - everything is visible
   Bool_t useFrustumCheck = (camera.FrustumOverlap(BoundingBox()) != kInside);

   glDisable(GL_BLEND);

   DrawListIt_t drawIt;
   for (drawIt = fDrawList.begin(); drawIt != fDrawList.end() && run;
        drawIt++) {
      drawShape = *drawIt;
      if (!drawShape)
      {
         assert(kFALSE);
         continue;
      }

      // Selected physical should always be drawn once only if visible, 
      // regardless of timeout object drop outs
      if (drawShape == fSelectedPhysical) {
         doSelected = kFALSE;
      }

      // TODO: Do small skipping first? Probably cheaper than frustum check
      // Profile relative costs? The frustum check could be done implictly 
      // from the LOD as we project all 8 verticies of the BB onto viewport

      // Work out if we need to draw this shape - assume we do first
      Bool_t drawNeeded = kTRUE;
      EOverlap overlap;

      // Draw test against passed clipping planes
      // Do before camera clipping on assumption clip planes remove more objects
      if (clipPlanes) {
         for (UInt_t i = 0; i < clipPlanes->size(); i++) {
            overlap = drawShape->BoundingBox().Overlap((*clipPlanes)[i]);
            if (overlap == kOutside) {
               drawNeeded = kFALSE;
               break;
            }
         }
      }

      // Draw test against camera frustum if require
      if (drawNeeded && useFrustumCheck)
      {
         overlap = camera.FrustumOverlap(drawShape->BoundingBox());
         drawNeeded = overlap == kInside || overlap == kPartial;
      }

      // Draw?
      if (drawNeeded)
      {
         // Collect transparent shapes and draw after opaque
         if (drawShape->IsTransparent()) {
            transDrawList.push_back(drawShape);
            continue;
         }

         // Get the shape draw quality
         UInt_t shapeLOD = CalcPhysicalLOD(*drawShape, camera, LOD);

         //Draw, DrawWireFrame, DrawOutline
         (drawShape->*drawPtr)(shapeLOD);
         UpdateDrawStats(*drawShape);
      }

      // Terminate the draw if over opaque fraction timeout
      if (timeout > 0.0) {
         Double_t opaqueTimeFraction = 1.0;
         if (fDrawStats.fOpaque > 0) {
            opaqueTimeFraction = (transDrawList.size() + fDrawStats.fOpaque) / fDrawStats.fOpaque; 
         }
         if (stopwatch.Lap() * opaqueTimeFraction > timeout) {
            run = kFALSE;
         }   
      }
   }

   // Step 2: Deal with selected physical in case skipped by timeout of above loop
   if (doSelected) {
      // Draw now if non-transparent
      if (!fSelectedPhysical->IsTransparent()) {
         UInt_t shapeLOD = CalcPhysicalLOD(*fSelectedPhysical, camera, LOD);
         (fSelectedPhysical->*drawPtr)(shapeLOD);
         UpdateDrawStats(*fSelectedPhysical);
      } else {
         // Add to transparent drawlist
         transDrawList.push_back(fSelectedPhysical);
      }
   }

   // Step 3: Draw the filtered transparent objects with GL depth writing off
   // blending on
   // TODO: Sort to draw back to front with depth test off for better blending
   glDepthMask(GL_FALSE);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

   for (drawIt = transDrawList.begin(); drawIt != transDrawList.end(); drawIt++) {
      drawShape = *drawIt;

      UInt_t shapeLOD = CalcPhysicalLOD(*drawShape, camera, LOD);

      //Draw, DrawWireFrame, DrawOutline
      (drawShape->*drawPtr)(shapeLOD);
      UpdateDrawStats(*drawShape);
   }

   // Reset these after transparent done
   glDepthMask(GL_TRUE);
   glDisable(GL_BLEND);

   // Finally: Draw selected object bounding box
   if (fSelectedPhysical) {

      //BBOX is white for wireframe mode and fill,
      //red for outlines
      switch (style) {
      case kFill:
      case kWireFrame :
         glColor3d(1., 1., 1.);

         break;
      case kOutline:
         glColor3d(1., 0., 0.);
         
         break;
      }

      if (style == kFill || style == kOutline) {
         glDisable(GL_LIGHTING);
      }
      glDisable(GL_DEPTH_TEST);

      fSelectedPhysical->BoundingBox().Draw();

      if (style == kFill || style == kOutline) {
         glEnable(GL_LIGHTING);
      }
      glEnable(GL_DEPTH_TEST);
   }

   return;
}

//______________________________________________________________________________
void TGLScene::SortDrawList()
{
   assert(!fDrawListValid);

   TGLStopwatch stopwatch;

   if (gDebug>2) {
      stopwatch.Start();
   }

   fDrawList.reserve(fPhysicalShapes.size());

   // Delete all zero (to-be-deleted) objects
   fDrawList.erase(remove(fDrawList.begin(), fDrawList.end(), static_cast<const TGLPhysicalShape *>(0)), fDrawList.end());
   
   assert(fDrawList.size() == fPhysicalShapes.size());

   //TODO: partition the selected to front

   // Sort by volume of shape bounding box
   sort(fDrawList.begin(), fDrawList.end(), TGLScene::ComparePhysicalVolumes);

   if (gDebug>2) {
      Info("TGLScene::SortDrawList", "sorting took %f msec", stopwatch.End());
   }

   fDrawListValid = kTRUE;
}

//______________________________________________________________________________
Bool_t TGLScene::ComparePhysicalVolumes(const TGLPhysicalShape * shape1, const TGLPhysicalShape * shape2)
{
   return (shape1->BoundingBox().Volume() > shape2->BoundingBox().Volume());
}

//______________________________________________________________________________
void TGLScene::DrawGuides(const TGLCamera & camera, EAxesType axesType, const TGLVertex3 * reference) const
{
   if (fLock != kDrawLock && fLock != kSelectLock) {
      Error("TGLScene::DrawMarkers", "expected Draw or Select Lock");
   }

   // Reference and origin based axes are not depth clipped
   glDisable(GL_DEPTH_TEST);

   // Draw any passed reference marker
   if (reference) {
      const Float_t referenceColor[4] = { 0.98, 0.45, 0.0, 1.0 }; // Orange
      TGLVector3 referenceSize = camera.ViewportDeltaToWorld(*reference, 3, 3);
      TGLUtil::DrawSphere(*reference, referenceSize.Mag(), referenceColor);
   }

   if (axesType != kAxesOrigin) {
      glEnable(GL_DEPTH_TEST);
   }
   if (axesType == kAxesNone) {
      return;
   }

   const Float_t axesColors[][4] = {{0.5, 0.0, 0.0, 1.0},  // -ive X axis light red 
                                    {1.0, 0.0, 0.0, 1.0},  // +ive X axis deep red
                                    {0.0, 0.5, 0.0, 1.0},  // -ive Y axis light green 
                                    {0.0, 1.0, 0.0, 1.0},  // +ive Y axis deep green
                                    {0.0, 0.0, 0.5, 1.0},  // -ive Z axis light blue 
                                    {0.0, 0.0, 1.0, 1.0}}; // +ive Z axis deep blue
   

   // Axes draw at fixed screen size - back project to world
   TGLVector3 pixelVector = camera.ViewportDeltaToWorld(BoundingBox().Center(), 1, 1);
   Double_t pixelSize = pixelVector.Mag();
   
   // Find x/y/z min/max values
   Double_t min[3] = { BoundingBox().XMin(), BoundingBox().YMin(), BoundingBox().ZMin() };
   Double_t max[3] = { BoundingBox().XMax(), BoundingBox().YMax(), BoundingBox().ZMax() };

   for (UInt_t i = 0; i < 3; i++) {
      TGLVertex3 start;
      TGLVector3 vector;
   
      if (axesType == kAxesOrigin) {
         // Through origin axes
         start[(i+1)%3] = 0.0;
         start[(i+2)%3] = 0.0;
      } else {
         // Side axes
         start[(i+1)%3] = min[(i+1)%3];
         start[(i+2)%3] = min[(i+2)%3];
      }
      vector[(i+1)%3] = 0.0;
      vector[(i+2)%3] = 0.0;

      // -ive axis?
      if (min[i] < 0.0) {
         // Runs from origin?
         if (max[i] > 0.0) {
            start[i] = 0.0;
            vector[i] = min[i];
         } else {
            start[i] = max[i];
            vector[i] = min[i] - max[i];
         }
         TGLUtil::DrawLine(start, vector, TGLUtil::kLineHeadNone, pixelSize*2.5, axesColors[i*2]);
      }
      // +ive axis?
      if (max[i] > 0.0) {
         // Runs from origin?
         if (min[i] < 0.0) {
            start[i] = 0.0;
            vector[i] = max[i];
         } else {
            start[i] = min[i];
            vector[i] = max[i] - min[i];
         }
         TGLUtil::DrawLine(start, vector, TGLUtil::kLineHeadNone, pixelSize*2.5, axesColors[i*2 + 1]);
      }
   }

   // Draw origin sphere(s)
   if (axesType == kAxesOrigin) {
      // Single white origin sphere at 0, 0, 0
      Float_t white[4] = { 1.0, 1.0, 1.0, 1.0 };
      TGLUtil::DrawSphere(TGLVertex3(0.0, 0.0, 0.0), pixelSize*2.0, white);  
   } else {
      for (UInt_t j = 0; j < 3; j++) {
         if (min[j] <= 0.0 && max[j] >= 0.0) {
            TGLVertex3 zero;
            zero[j] = 0.0; 
            zero[(j+1)%3] = min[(j+1)%3];
            zero[(j+2)%3] = min[(j+2)%3];
            TGLUtil::DrawSphere(zero, pixelSize*2.0, axesColors[j*2 + 1]);
         }
      }
   }

   static const UChar_t xyz[][8] = {{0x44, 0x44, 0x28, 0x10, 0x10, 0x28, 0x44, 0x44},
                                    {0x10, 0x10, 0x10, 0x10, 0x10, 0x28, 0x44, 0x44},
                                    {0x7c, 0x20, 0x10, 0x10, 0x08, 0x08, 0x04, 0x7c}};

   glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

   // Labels
   Double_t padPixels = 25.0;

   glDisable(GL_LIGHTING);
   for (UInt_t k = 0; k < 3; k++) {
      TGLUtil::SetDrawColors(axesColors[k*2+1]);
      TGLVertex3 minPos, maxPos;
      if (axesType == kAxesOrigin) {
         minPos[(k+1)%3] = 0.0;
         minPos[(k+2)%3] = 0.0;
      } else {
         minPos[(k+1)%3] = min[(k+1)%3];
         minPos[(k+2)%3] = min[(k+2)%3];
      }
      maxPos = minPos;
      minPos[k] = min[k];
      maxPos[k] = max[k];

      TGLVector3 axis = maxPos - minPos;
      TGLVector3 axisViewport = camera.WorldDeltaToViewport(minPos, axis);

      // Skip drawning if viewport projection of axis very small - labels will overlap
      // Occurs with orthographic cameras
      if (axisViewport.Mag() < 1) { 
         continue;
      }

      minPos -= camera.ViewportDeltaToWorld(minPos, padPixels*axisViewport.X()/axisViewport.Mag(), 
                                                    padPixels*axisViewport.Y()/axisViewport.Mag());
      axisViewport = camera.WorldDeltaToViewport(maxPos, -axis);
      maxPos -= camera.ViewportDeltaToWorld(maxPos, padPixels*axisViewport.X()/axisViewport.Mag(),
                                                    padPixels*axisViewport.Y()/axisViewport.Mag());

      DrawNumber(min[k], minPos);        // Min value
      DrawNumber(max[k], maxPos);        // Max value
   
      // Axis name beside max value
      TGLVertex3 namePos = maxPos - 
         camera.ViewportDeltaToWorld(maxPos, padPixels*axisViewport.X()/axisViewport.Mag(),
                                     padPixels*axisViewport.Y()/axisViewport.Mag());
      glRasterPos3dv(namePos.CArr());
      glBitmap(8, 8, 0.0, 4.0, 0.0, 0.0, xyz[k]); // Axis Name
   }
   glEnable(GL_LIGHTING);
   glEnable(GL_DEPTH_TEST);
}

//______________________________________________________________________________
void TGLScene::DrawNumber(Double_t num, const TGLVertex3 & center) const
{
   if (fLock != kDrawLock && fLock != kSelectLock) {
      Error("TGLScene::DrawNumber", "expected Draw or Select Lock");
   }
   static const UChar_t
      digits[][8] = {{0x38, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x38},//0
                     {0x10, 0x10, 0x10, 0x10, 0x10, 0x70, 0x10, 0x10},//1
                     {0x7c, 0x44, 0x20, 0x18, 0x04, 0x04, 0x44, 0x38},//2
                     {0x38, 0x44, 0x04, 0x04, 0x18, 0x04, 0x44, 0x38},//3
                     {0x04, 0x04, 0x04, 0x04, 0x7c, 0x44, 0x44, 0x44},//4
                     {0x7c, 0x44, 0x04, 0x04, 0x7c, 0x40, 0x40, 0x7c},//5
                     {0x7c, 0x44, 0x44, 0x44, 0x7c, 0x40, 0x40, 0x7c},//6
                     {0x20, 0x20, 0x20, 0x10, 0x08, 0x04, 0x44, 0x7c},//7
                     {0x38, 0x44, 0x44, 0x44, 0x38, 0x44, 0x44, 0x38},//8
                     {0x7c, 0x44, 0x04, 0x04, 0x7c, 0x44, 0x44, 0x7c},//9
                     {0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},//.
                     {0x00, 0x00, 0x00, 0x00, 0x7c, 0x00, 0x00, 0x00}};//-

   TString str;
   str+=Long_t(num);
   Double_t xOffset = 3.5 * str.Length();
   Double_t yOffset = 4.0;
   glRasterPos3dv(center.CArr());
   for (Ssiz_t i = 0, e = str.Length(); i < e; ++i) {
      if (str[i] == '.') {
         glBitmap(8, 8, xOffset, yOffset, 7.0, 0.0, digits[10]);
      } else if (str[i] == '-') {
         glBitmap(8, 8, xOffset, yOffset, 7.0, 0.0, digits[11]);
      } else {
         glBitmap(8, 8, xOffset, yOffset, 7.0, 0.0, digits[str[i] - '0']);
      }
   }
}

//______________________________________________________________________________
UInt_t TGLScene:: CalcPhysicalLOD(const TGLPhysicalShape & shape, const TGLCamera & camera,
                                 UInt_t sceneLOD) const
{
   // Find diagonal pixel size of projected drawable BB, using camera
   Double_t diagonal = static_cast<Double_t>(camera.ViewportSize(shape.BoundingBox()).Diagonal());

   // TODO: Get real screen size - assuming 2000 pixel screen at present
   // Calculate a non-linear sizing hint for this shape. Needs more experimenting with...
   UInt_t sizeLOD = static_cast<UInt_t>(pow(diagonal,0.4) * 100.0 / pow(2000.0,0.4));

   // Factor in scene quality
   UInt_t shapeLOD = (sceneLOD * sizeLOD) / 100;

   if (shapeLOD > 10) {
      Double_t quant = ((static_cast<Double_t>(shapeLOD)) + 0.3) / 10;
      shapeLOD = static_cast<UInt_t>(quant)*10;
   } else {
      Double_t quant = ((static_cast<Double_t>(shapeLOD)) + 0.3) / 3;
      shapeLOD = static_cast<UInt_t>(quant)*3;
   }

   if (shapeLOD > 100) {
      shapeLOD = 100;
   }

   return shapeLOD;
}

//______________________________________________________________________________
Bool_t TGLScene::Select(const TGLCamera & camera, EDrawStyle style, const TGLClip * clip)
{
   Bool_t changed = kFALSE;
   if (fLock != kSelectLock) {
      Error("TGLScene::Select", "expected SelectLock");
   }

   // Create the select buffer. This will work as we have a flat set of physical shapes.
   // We only ever load a single name in TGLPhysicalShape::DirectDraw so any hit record always
   // has same 4 GLuint format
   static std::vector<GLuint> selectBuffer;
   selectBuffer.resize(fPhysicalShapes.size()*4);
   glSelectBuffer(selectBuffer.size(), &selectBuffer[0]);

   // Enter picking mode
   glRenderMode(GL_SELECT);
   glInitNames();
   glPushName(0);

   // Draw out scene at best quality with clipping, no timelimit
   Draw(camera, style, kHigh, 0.0, clip);

   // Retrieve the hit count and return to render
   Int_t hits = glRenderMode(GL_RENDER);

   if (hits < 0) {
      Error("TGLScene::Select", "selection buffer overflow");
      return changed;
   }

   if (hits > 0) {
      // Every hit record has format (GLuint per item) - format is:
      //
      // no of names in name block (1 always)
      // minDepth
      // maxDepth
      // name(s) (1 always)

      // Sort the hits by minimum depth (closest part of object)
      static std::vector<std::pair<UInt_t, Int_t> > sortedHits;
      sortedHits.resize(hits);
      Int_t i;
      for (i = 0; i < hits; i++) {
         assert(selectBuffer[i * 4] == 1); // expect a single name per record
         sortedHits[i].first = selectBuffer[i * 4 + 1]; // hit object minimum depth
         sortedHits[i].second = selectBuffer[i * 4 + 3]; // hit object name
      }
      std::sort(sortedHits.begin(), sortedHits.end());

      // Find first (closest) non-transparent object in the hit stack
      TGLPhysicalShape * selected = 0;
      for (i = 0; i < hits; i++) {
         selected = FindPhysical(sortedHits[i].second);
         if (!selected->IsTransparent()) {
            break;
         }
      }
      // If we failed to find a non-transparent object use the first
      // (closest) transparent one
      if (selected->IsTransparent()) {
         selected = FindPhysical(sortedHits[0].second);
      }
      assert(selected);

      // Swap any selection
      if (selected != fSelectedPhysical) {
         if (fSelectedPhysical) {
            fSelectedPhysical->Select(kFALSE);
         }
         fSelectedPhysical = selected;
         fSelectedPhysical->Select(kTRUE);
         changed = kTRUE;
      }
   } else { // 0 hits
      if (fSelectedPhysical) {
         fSelectedPhysical->Select(kFALSE);
         fSelectedPhysical = 0;
         changed = kTRUE;
      }
   }

   return changed;
}

//______________________________________________________________________________
Bool_t TGLScene::SetSelectedColor(const Float_t color[17])
{
   if (fSelectedPhysical) {
      fSelectedPhysical->SetColor(color);
      return kTRUE;
   } else {
      assert(kFALSE);
      return kFALSE;
   }
}

//______________________________________________________________________________
Bool_t TGLScene::SetColorOnSelectedFamily(const Float_t color[17])
{
   if (fSelectedPhysical) {
      TGLPhysicalShape * physical;
      PhysicalShapeMapIt_t physicalShapeIt = fPhysicalShapes.begin();
      while (physicalShapeIt != fPhysicalShapes.end()) {
         physical = physicalShapeIt->second;
         if (physical) {
            if (physical->GetLogical().ID() == fSelectedPhysical->GetLogical().ID()) {
               physical->SetColor(color);
            }
         } else {
            assert(kFALSE);
         }
         ++physicalShapeIt;
      }
      return kTRUE;
   } else {
      assert(kFALSE);
      return kFALSE;
   }
}

//______________________________________________________________________________
Bool_t TGLScene::ShiftSelected(const TGLVector3 & shift)
{
   if (fSelectedPhysical) {
      fSelectedPhysical->Translate(shift);
      fBoundingBoxValid = kFALSE;
      return kTRUE;
   }
   else {
      assert(kFALSE);
      return kFALSE;
   }
}

//______________________________________________________________________________
Bool_t TGLScene::SetSelectedGeom(const TGLVertex3 & trans, const TGLVector3 & scale)
{
   if (fSelectedPhysical) {
      fSelectedPhysical->SetTranslation(trans);
      fSelectedPhysical->Scale(scale);
      fBoundingBoxValid = kFALSE;
      return kTRUE;
   } else {
      assert(kFALSE);
      return kFALSE;
   }
}

//______________________________________________________________________________
const TGLBoundingBox & TGLScene::BoundingBox() const
{
   if (!fBoundingBoxValid) {
      Double_t xMin, xMax, yMin, yMax, zMin, zMax;
      xMin = xMax = yMin = yMax = zMin = zMax = 0.0;
      PhysicalShapeMapCIt_t physicalShapeIt = fPhysicalShapes.begin();
      const TGLPhysicalShape * physicalShape;
      while (physicalShapeIt != fPhysicalShapes.end())
      {
         physicalShape = physicalShapeIt->second;
         if (!physicalShape)
         {
            assert(kFALSE);
            continue;
         }
         TGLBoundingBox box = physicalShape->BoundingBox();
         if (physicalShapeIt == fPhysicalShapes.begin()) {
            xMin = box.XMin(); xMax = box.XMax();
            yMin = box.YMin(); yMax = box.YMax();
            zMin = box.ZMin(); zMax = box.ZMax();
         } else {
            if (box.XMin() < xMin) { xMin = box.XMin(); }
            if (box.XMax() > xMax) { xMax = box.XMax(); }
            if (box.YMin() < yMin) { yMin = box.YMin(); }
            if (box.YMax() > yMax) { yMax = box.YMax(); }
            if (box.ZMin() < zMin) { zMin = box.ZMin(); }
            if (box.ZMax() > zMax) { zMax = box.ZMax(); }
         }
         ++physicalShapeIt;
      }
      fBoundingBox.SetAligned(TGLVertex3(xMin,yMin,zMin), TGLVertex3(xMax,yMax,zMax));
      fBoundingBoxValid = kTRUE;
   }
   return fBoundingBox;
}

//______________________________________________________________________________
void TGLScene::Dump() const
{
   std::cout << "Scene: " << fLogicalShapes.size() << " Logicals / " << fPhysicalShapes.size() << " Physicals " << std::endl;
}

//______________________________________________________________________________
UInt_t TGLScene::SizeOf() const
{
   UInt_t size = sizeof(this);

   std::cout << "Size: Scene Only " << size << std::endl;

   LogicalShapeMapCIt_t logicalShapeIt = fLogicalShapes.begin();
   const TGLLogicalShape * logicalShape;
   while (logicalShapeIt != fLogicalShapes.end()) {
      logicalShape = logicalShapeIt->second;
      size += sizeof(*logicalShape);
      ++logicalShapeIt;
   }

   std::cout << "Size: Scene + Logical Shapes " << size << std::endl;

   PhysicalShapeMapCIt_t physicalShapeIt = fPhysicalShapes.begin();
   const TGLPhysicalShape * physicalShape;
   while (physicalShapeIt != fPhysicalShapes.end()) {
      physicalShape = physicalShapeIt->second;
      size += sizeof(*physicalShape);
      ++physicalShapeIt;
   }

   std::cout << "Size: Scene + Logical Shapes + Physical Shapes " << size << std::endl;

   return size;
}

//______________________________________________________________________________
void TGLScene::ResetDrawStats()
{
   fDrawStats.fOpaque = 0;
   fDrawStats.fTrans = 0;
   fDrawStats.fByShape.clear();
}

//______________________________________________________________________________
void TGLScene::UpdateDrawStats(const TGLPhysicalShape & shape)
{
   // Update opaque/transparent draw count
   if (shape.IsTransparent()) {
      ++fDrawStats.fTrans;
   } else {
      ++fDrawStats.fOpaque;
   }

   // By type only needed for debug currently
   if (gDebug>3) {
      // Update the stats 
      std::string shapeType = shape.GetLogical().IsA()->GetName();
      typedef std::map<std::string, UInt_t>::iterator MapIt_t;
      MapIt_t statIt = fDrawStats.fByShape.find(shapeType);

      if (statIt == fDrawStats.fByShape.end()) {
         //do not need to check insert(.....).second, because statIt was stats.end() before
         statIt = fDrawStats.fByShape.insert(std::make_pair(shapeType, 0u)).first;
      }

      statIt->second++;   
   }
}
 
//______________________________________________________________________________
void TGLScene::DumpDrawStats()
{
   // Dump some current draw stats for debuggin

   // Draw counts
   if (gDebug>2) {
      Info("TGLScene::DumpDrawStats()", "Drew %i, %i Opaque %i Transparent", fDrawStats.fOpaque + fDrawStats.fTrans,
         fDrawStats.fOpaque, fDrawStats.fTrans);
   }

   // By shape type counts
   if (gDebug>3) {
      std::map<std::string, UInt_t>::const_iterator it = fDrawStats.fByShape.begin();
      while (it != fDrawStats.fByShape.end()) {
         std::cout << it->first << " (" << it->second << ")\t";
         it++;
      }
      std::cout << std::endl;
   }
}
