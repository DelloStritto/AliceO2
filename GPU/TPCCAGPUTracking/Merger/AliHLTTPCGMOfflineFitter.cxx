// **************************************************************************
// This file is property of and copyright by the ALICE HLT Project          *
// ALICE Experiment at CERN, All rights reserved.                           *
//                                                                          *
// Primary Authors: Sergey Gorbunov <sergey.gorbunov@kip.uni-heidelberg.de> *
//                  for The ALICE HLT Project.                              *
//                                                                          *
// Permission to use, copy, modify and distribute this software and its     *
// documentation strictly for non-commercial purposes is hereby granted     *
// without fee, provided that the above copyright notice appears in all     *
// copies and that both the copyright notice and this permission notice     *
// appear in the supporting documentation. The authors make no claims       *
// about the suitability of this software for any purpose. It is            *
// provided "as is" without express or implied warranty.                    *
//                                                                          *
//***************************************************************************


#if ( !defined(HLTCA_STANDALONE) && !defined(HLTCA_GPUCODE) )


#include "AliHLTTPCGMOfflineFitter.h"

#include "AliHLTTPCCAMath.h"
#include "AliHLTTPCGMMergedTrack.h"
#include "AliHLTTPCGMMergedTrackHit.h"
#include "AliHLTTPCCAGeometry.h"
#include <cmath>
#include "AliTracker.h"
#include "AliMagF.h"
#include "AliExternalTrackParam.h"
#include "AliTPCtracker.h"
#include "AliTPCParam.h"
#include "AliTPCseed.h"
#include "AliTPCclusterMI.h"
#include "AliTPCcalibDB.h"
#include "AliTPCParamSR.h"
#include "AliHLTTPCGMPropagator.h"
#include "AliTPCReconstructor.h"
#include "AliHLTTPCClusterTransformation.h"

#define DOUBLE 1


AliHLTTPCGMOfflineFitter::AliHLTTPCGMOfflineFitter()
  :fCAParam()
{
}

AliHLTTPCGMOfflineFitter::~AliHLTTPCGMOfflineFitter()
{
}

void AliHLTTPCGMOfflineFitter::Initialize( const AliHLTTPCCAParam& hltParam, Long_t TimeStamp, bool isMC )
{
  //

  AliHLTTPCClusterTransformation hltTransform;
  hltTransform.Init( 0., TimeStamp, isMC, 1);  
 
  // initialisation of AliTPCtracker as it is done in AliTPCReconstructor.cxx 
  
  AliTPCcalibDB * calib = AliTPCcalibDB::Instance();
  const AliMagF * field = (AliMagF*)TGeoGlobalMagField::Instance()->GetField();
  calib->SetExBField(field);

  AliTPCParam* param = AliTPCcalibDB::Instance()->GetParameters();
  if (!param) {
    AliWarning("Loading default TPC parameters !");
    param = new AliTPCParamSR;
  }
  param->ReadGeoMatrices(); 

  AliTPCReconstructor *tpcRec = new AliTPCReconstructor();
  tpcRec->SetRecoParam( AliTPCcalibDB::Instance()->GetTransform()->GetCurrentRecoParam() );

  //(this)->~AliTPCtracker();   //call the destructor explicitly
  //new (this) AliTPCtracker(param); // call the constructor 

  AliTPCtracker::fSectors = AliTPCtracker::fInnerSec; 
  // AliTPCReconstructor::ParseOptions(tracker);  : not important, it only set useHLTClusters flag
  
  fCAParam = hltParam;
}



void AliHLTTPCGMOfflineFitter::RefitTrack( AliHLTTPCGMMergedTrack &track, const AliHLTTPCGMPolynomialField* field,  AliHLTTPCGMMergedTrackHit* clusters )
{

  // copy of HLT RefitTrack() with calling of the offline fit utilities

  if( !track.OK() ) return;    

  int nTrackHits = track.NClusters();
  cout<<"call FitOffline .. "<<endl;
  bool ok  = FitOffline( field, track, clusters + track.FirstClusterRef(), nTrackHits ); 
  cout<<".. end of call FitOffline "<<endl;

  AliHLTTPCGMTrackParam t = track.Param();
  float Alpha = track.Alpha();  
 
  if ( fabs( t.QPt() ) < 1.e-4 ) t.QPt() = 1.e-4 ;
	  
  track.SetOK(ok);
  track.SetNClustersFitted( nTrackHits );
  track.Param() = t;
  track.Alpha() = Alpha;

  {
    int ind = track.FirstClusterRef();
    float alphaa = fCAParam.Alpha(clusters[ind].fSlice);
    float xx = clusters[ind].fX;
    float yy = clusters[ind].fY;
    float zz = clusters[ind].fZ - track.Param().GetZOffset();
    float sinA = AliHLTTPCCAMath::Sin( alphaa - track.Alpha());
    float cosA = AliHLTTPCCAMath::Cos( alphaa - track.Alpha());
    track.SetLastX( xx*cosA - yy*sinA );
    track.SetLastY( xx*sinA + yy*cosA );
    track.SetLastZ( zz );
  }
}


int  AliHLTTPCGMOfflineFitter::CreateTPCclusterMI( const AliHLTTPCGMMergedTrackHit &h, AliTPCclusterMI &c)
{
  // Create AliTPCclusterMI for the HLT hit

  AliTPCclusterMI tmp; // everything is set to 0 by constructor
  c = tmp;

  // add the information we have

  Int_t sector, row;
  AliHLTTPCCAGeometry::Slice2Sector( h.fSlice, h.fRow, sector, row);
  c.SetDetector( sector );
  c.SetRow( row ); // ?? is it right row numbering for the TPC tracker ??  
  c.SetX(h.fX);
  c.SetY(h.fY);
  c.SetZ(h.fZ);
  int index=(((sector<<8)+row)<<16)+0;
  return index;
}    

bool AliHLTTPCGMOfflineFitter::FitOffline( const AliHLTTPCGMPolynomialField* field, AliHLTTPCGMMergedTrack &gmtrack,  AliHLTTPCGMMergedTrackHit* clusters, int &N )
{

  const float maxSinPhi = HLTCA_MAX_SIN_PHI;

  int maxN = N;
  float covYYUpd = 0.;
  float lastUpdateX = -1.;
  
  const bool rejectChi2ThisRound = 0;
  const bool markNonFittedClusters = 0;
  const double kDeg2Rad = 3.14159265358979323846/180.;
  const float maxSinForUpdate = CAMath::Sin(70.*kDeg2Rad);

  bool ok = 1;

  AliTPCtracker::SetIteration(2);

  AliTPCseed seed;
  gmtrack.Param().GetExtParam( seed, gmtrack.Alpha() );  

  AliTPCtracker::AddCovariance( &seed );
  
  N = 0;
  lastUpdateX = -1;

  // find last leg
  int ihitStart = 0;
  for( int ihit=0; ihit<maxN; ihit++ ){
    if( clusters[ihit].fLeg != clusters[ihitStart].fLeg ) ihitStart = ihit;
  }

  for( int ihit = ihitStart; ihit<maxN; ihit ++ ){

    if (clusters[ihit].fState < 0) continue; // hit is excluded from fit
    float xx = clusters[ihit].fX;
    float yy = clusters[ihit].fY;
    float zz = clusters[ihit].fZ;
   
    if (DOUBLE && ihit + 1 >= 0 && ihit + 1 < maxN && clusters[ihit].fRow == clusters[ihit + 1].fRow){
      float count = 1.;
      do{
	if (clusters[ihit].fSlice != clusters[ihit + 1].fSlice || clusters[ihit].fLeg != clusters[ihit + 1].fLeg || fabs(clusters[ihit].fY - clusters[ihit + 1].fY) > 4. || fabs(clusters[ihit].fZ - clusters[ihit + 1].fZ) > 4.) break;
	ihit += 1;
	xx += clusters[ihit].fX;
	yy += clusters[ihit].fY;
	zz += clusters[ihit].fZ;
	count += 1.;
      } while (ihit + 1 >= 0 && ihit + 1 < maxN && clusters[ihit].fRow == clusters[ihit + 1].fRow);
      xx /= count;
      yy /= count;
      zz /= count;          
    }
    
    // Create AliTPCclusterMI for the hit
    
    AliTPCclusterMI cluster;
    Int_t tpcindex = CreateTPCclusterMI( clusters[ihit], cluster );
    if( tpcindex <0 ) continue;
    Double_t sy2=0, sz2=0;
    AliTPCtracker::ErrY2Z2( &seed, &cluster,sy2,sz2);      
    cluster.SetSigmaY2( sy2 );
    cluster.SetSigmaZ2( sz2 );
    cluster.SetQ(10);
    cluster.SetMax(10);
    
    Int_t iRow = clusters[ihit].fRow;

    if( iRow < AliHLTTPCCAGeometry::GetNRowLow() ) AliTPCtracker::fSectors = AliTPCtracker::fInnerSec; 
    else AliTPCtracker::fSectors = AliTPCtracker::fOuterSec; 

    seed.SetClusterIndex2( iRow, tpcindex );
    seed.SetClusterPointer( iRow, &cluster );
    seed.SetCurrentClusterIndex1(tpcindex); 

    int retVal;
    float threshold = 3. + (lastUpdateX >= 0 ? (fabs(seed.GetX() - lastUpdateX) / 2) : 0.);
    if (N > 2 && (fabs(yy - seed.GetY()) > threshold || fabs(zz - seed.GetZ()) > threshold)) retVal = 2;
    else{
     
      Int_t  err = !( AliTPCtracker::FollowToNext( seed, iRow) );
    
      const int err2 = N > 0 && CAMath::Abs(seed.GetSnp())>=maxSinForUpdate;
      if ( err || err2 ){
	if (markNonFittedClusters)
	  {
	    if (N > 0 && (fabs(yy - seed.GetY()) > 3 || fabs(zz - seed.GetZ()) > 3)) clusters[ihit].fState = -2;
	    else if (err && err >= -3) clusters[ihit].fState = -1;
	  }        
	continue;
      }
      
      // retVal = prop.Update( yy, zz, clusters[ihit].fRow, param, rejectChi2ThisRound);
      retVal = 0;
      
    }

    if (retVal == 0) // track is updated
      {
        lastUpdateX = seed.GetX();
        covYYUpd = seed.GetCovariance()[0];
        ihitStart = ihit;
        N++;
      }
    else if (retVal == 2) // cluster far away form the track
      {
        if (markNonFittedClusters) clusters[ihit].fState = -2;
      }
    else break; // bad chi2 for the whole track, stop the fit
  } // end loop over clusters   


 
  AliHLTTPCGMTrackParam t;
  t.SetExtParam( seed  );  

  float Alpha = seed.GetAlpha();

  t.ConstrainSinPhi();

  bool ok1 = N >= TRACKLET_SELECTOR_MIN_HITS(t.GetQPt()) && t.CheckNumericalQuality(covYYUpd);
  if (!ok1) return(false);

  
  //   const float kDeg2Rad = 3.1415926535897 / 180.f;
  const float kSectAngle = 2*3.1415926535897 / 18.f;


  if (fCAParam.GetTrackReferenceX() <= 500)
  {

    AliHLTTPCGMPropagator prop;
    //    prop.SetMaterial( kRadLen, kRho );
    prop.SetPolynomialField( field );  
    prop.SetMaxSinPhi( maxSinPhi );
    prop.SetToyMCEventsFlag( fCAParam.ToyMCEventsFlag());

    for (int k = 0;k < 3;k++) //max 3 attempts
      {
	int err = prop.PropagateToXAlpha(fCAParam.GetTrackReferenceX(), Alpha, 0);
	t.ConstrainSinPhi();
	if (fabs(t.GetY()) <= t.GetX() * tan(kSectAngle / 2.f)) break;
	float dAngle = floor(atan2(t.GetY(), t.GetX()) / kDeg2Rad / 20.f + 0.5f) * kSectAngle;
	Alpha += dAngle;
	if (err || k == 2)
          {
	    t.Rotate(dAngle);
	    break;
          }
      }    
  }
  else if (fabs(t.GetY()) > t.GetX() * tan(kSectAngle / 2.f))
  {
    float dAngle = floor(atan2(t.GetY(), t.GetX()) / kDeg2Rad / 20.f + 0.5f) * kSectAngle;
      t.Rotate(dAngle);
      Alpha += dAngle;
  }
  if (Alpha > 3.1415926535897) Alpha -= 2*3.1415926535897;
  else if (Alpha <= -3.1415926535897) Alpha += 2*3.1415926535897;

  gmtrack.Param() = t ;
  gmtrack.Alpha() = Alpha;  

  return(ok);
}



#endif
