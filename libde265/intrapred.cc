/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libde265.
 *
 * libde265 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libde265 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libde265.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "intrapred.h"
#include "transform.h"
#include "util.h"
#include <assert.h>


#include <sys/types.h>
#include <string.h>


int nIntraPredictions;
int nAvail0;
int nAvailPart;
int nAvailAll;
int nAvailSz[32*2+32*2+1+1];

extern "C" {
LIBDE265_API void showIntraPredictionProfile()
{
  printf("nIntraPredictions: %d\n", nIntraPredictions);
  printf("  with no available border samples: %d\n", nAvail0);
  printf("  with partially available samples: %d\n", nAvailPart);
  printf("  with complete border samples: %d\n", nAvailAll);

  if (0) {
    printf("  ");
    for (int i=0;i<32*2+32*2+1+1;i++)
      printf("%d ",nAvailSz[i]);
    printf("\n");
  }
}
}


#ifdef DE265_LOG_TRACE
void print_border(uint8_t* data, uint8_t* available, int nT)
{
  for (int i=-2*nT ; i<=2*nT ; i++) {
    if (i==0 || i==1 || i==-nT || i==nT+1) {
      logtrace(LogIntraPred,"|");
    } else {
      logtrace(LogIntraPred," ");
    }

    if (available==NULL || available[i]) {
      logtrace(LogIntraPred,"%02x",data[i]);
    }
    else {
      logtrace(LogIntraPred,"--");
    }
  }
}
#else
#define print_border(data, available, nT)
#endif


void fillIntraPredModeCandidates(int candModeList[3], int x,int y, int PUidx,
                                 bool availableA, // left
                                 bool availableB, // top
                                 const de265_image* img)
{
  const seq_parameter_set* sps = &img->sps;

  // block on left side

  enum IntraPredMode candIntraPredModeA, candIntraPredModeB;
  if (availableA==false) {
    candIntraPredModeA=INTRA_DC;
  }
  else if (img->get_pred_mode(x-1,y) != MODE_INTRA ||
           img->get_pcm_flag (x-1,y)) {
    candIntraPredModeA=INTRA_DC;
  }
  else {
    candIntraPredModeA = img->get_IntraPredMode_atIndex(PUidx-1);
  }

  // block above

  if (availableB==false) {
    candIntraPredModeB=INTRA_DC;
  }
  else if (img->get_pred_mode(x,y-1) != MODE_INTRA ||
           img->get_pcm_flag (x,y-1)) {
    candIntraPredModeB=INTRA_DC;
  }
  else if (y-1 < ((y >> sps->Log2CtbSizeY) << sps->Log2CtbSizeY)) {
    candIntraPredModeB=INTRA_DC;
  }
  else {
    candIntraPredModeB = img->get_IntraPredMode_atIndex(PUidx-sps->PicWidthInMinPUs);
  }


  // build candidate list

  logtrace(LogSlice,"availableA:%d candA:%d & availableB:%d candB:%d\n",
           availableA, candIntraPredModeA,
           availableB, candIntraPredModeB);

  if (candIntraPredModeA == candIntraPredModeB) {
    if (candIntraPredModeA < 2) {
      candModeList[0] = INTRA_PLANAR;
      candModeList[1] = INTRA_DC;
      candModeList[2] = INTRA_ANGULAR_26; 
    }
    else {
      candModeList[0] = candIntraPredModeA;
      candModeList[1] = 2 + ((candIntraPredModeA-2 -1 +32) % 32);
      candModeList[2] = 2 + ((candIntraPredModeA-2 +1    ) % 32);
    }
  }
  else {
    candModeList[0] = candIntraPredModeA;
    candModeList[1] = candIntraPredModeB;

    if (candIntraPredModeA != INTRA_PLANAR &&
        candIntraPredModeB != INTRA_PLANAR) {
      candModeList[2] = INTRA_PLANAR;
    }
    else if (candIntraPredModeA != INTRA_DC &&
             candIntraPredModeB != INTRA_DC) {
      candModeList[2] = INTRA_DC;
    }
    else {
      candModeList[2] = INTRA_ANGULAR_26; 
    }
  }

  /*
  printf("candModeList: %d %d %d\n",
         candModeList[0],
         candModeList[1],
         candModeList[2]
         );
  */
}


int find_intra_pred_mode(enum IntraPredMode mode,
                         int candModeList[3])
{
  // check whether the mode is in the candidate list

  for (int i=0;i<3;i++) {
    if (candModeList[i] == mode) {
      return i;
    }
  }

  // sort candModeList

  if (candModeList[0] > candModeList[1]) {
    std::swap(candModeList[0],candModeList[1]);
  }
  if (candModeList[0] > candModeList[2]) {
    std::swap(candModeList[0],candModeList[2]);
  }
  if (candModeList[1] > candModeList[2]) {
    std::swap(candModeList[1],candModeList[2]);
  }

  // skip modes already in the candidate list

  int intraMode = mode;

  for (int i=2;i>=0;i--) {
    if (intraMode >= candModeList[i]) { intraMode--; }
  }

  return -intraMode-1;
}


void list_chroma_pred_candidates(enum IntraPredMode chroma_mode[5],
                                 enum IntraPredMode luma_mode)
{
  enum IntraPredMode chroma_cand[5];
  chroma_cand[0] = INTRA_PLANAR;
  chroma_cand[1] = INTRA_ANGULAR_26;
  chroma_cand[2] = INTRA_ANGULAR_10;
  chroma_cand[3] = INTRA_DC;
  chroma_cand[4] = luma_mode;

  switch (luma_mode) {
  case INTRA_PLANAR:     chroma_cand[0] = INTRA_ANGULAR_34; break;
  case INTRA_ANGULAR_26: chroma_cand[1] = INTRA_ANGULAR_34; break;
  case INTRA_ANGULAR_10: chroma_cand[2] = INTRA_ANGULAR_34; break;
  case INTRA_DC:         chroma_cand[3] = INTRA_ANGULAR_34; break;
  }
}


int get_intra_scan_idx_luma(int log2TrafoSize, enum IntraPredMode intraPredMode)
{
  if (log2TrafoSize==2 || log2TrafoSize==3) {
    /**/ if (intraPredMode >=  6 && intraPredMode <= 14) return 2;
    else if (intraPredMode >= 22 && intraPredMode <= 30) return 1;
    else return 0;
  }
  else { return 0; }
}

int get_intra_scan_idx_chroma(int log2TrafoSize, enum IntraPredMode intraPredMode)
{
  if (log2TrafoSize==1 || log2TrafoSize==2) {
    /**/ if (intraPredMode >=  6 && intraPredMode <= 14) return 2;
    else if (intraPredMode >= 22 && intraPredMode <= 30) return 1;
    else return 0;
  }
  else { return 0; }
}


// (8.4.4.2.2)
void fill_border_samples(de265_image* img, int xB,int yB,
                         int nT, int cIdx,
                         uint8_t* out_border)
{
  const seq_parameter_set* sps = &img->sps;
  const pic_parameter_set* pps = &img->pps;

  uint8_t available_data[2*64 + 1];
  uint8_t* available = &available_data[64];

  uint8_t* image;
  int stride;
  image  = img->get_image_plane(cIdx);
  stride = img->get_image_stride(cIdx);

  const int chromaShift = (cIdx==0) ? 0 : 1;
  const int TUShift = (cIdx==0) ? sps->Log2MinTrafoSize : sps->Log2MinTrafoSize-1;


  // --- check for CTB boundaries ---

  int xBLuma = (cIdx==0) ? xB : 2*xB;
  int yBLuma = (cIdx==0) ? yB : 2*yB;
  int nTLuma = (cIdx==0) ? nT : 2*nT;

  int log2CtbSize = sps->Log2CtbSizeY;
  int picWidthInCtbs = sps->PicWidthInCtbsY;

  bool availableLeft=true;    // is CTB at left side available?
  bool availableTop=true;     // is CTB at top side available?
  bool availableTopRight=true; // is CTB at top-right side available?
  bool availableTopLeft=true;  // if CTB at top-left pixel available?


  // are we at left image border

  if (xBLuma == 0) {
    availableLeft = false;
    availableTopLeft = false;
    xBLuma = 0; // fake value, available flags are already set to false
  }


  // are we at top image border

  if (yBLuma == 0) {
    availableTop = false;
    availableTopLeft = false;
    availableTopRight = false;
    yBLuma = 0; // fake value, available flags are already set to false
  }

  if (xBLuma+nTLuma >= sps->pic_width_in_luma_samples) {
    availableTopRight=false;
  }
 
  // check for tile and slice boundaries

  int xCurrCtb = xBLuma >> log2CtbSize;
  int yCurrCtb = yBLuma >> log2CtbSize;
  int xLeftCtb = (xBLuma-1) >> log2CtbSize;
  int xRightCtb = (xBLuma+nTLuma) >> log2CtbSize;
  int yTopCtb   = (yBLuma-1) >> log2CtbSize;

  int currCTBSlice = img->get_SliceAddrRS(xCurrCtb,yCurrCtb);
  int leftCTBSlice = availableLeft ? img->get_SliceAddrRS(xLeftCtb, yCurrCtb) : -1;
  int topCTBSlice  = availableTop ? img->get_SliceAddrRS(xCurrCtb, yTopCtb) : -1;
  int toprightCTBSlice = availableTopRight ? img->get_SliceAddrRS(xRightCtb, yTopCtb) : -1;
  int topleftCTBSlice  = availableTopLeft  ? img->get_SliceAddrRS(xLeftCtb, yTopCtb) : -1;

  int currCTBTileID = pps->TileIdRS[xCurrCtb+yCurrCtb*picWidthInCtbs];
  int leftCTBTileID = availableLeft ? pps->TileIdRS[xLeftCtb+yCurrCtb*picWidthInCtbs] : -1;
  int topCTBTileID  = availableTop ? pps->TileIdRS[xCurrCtb+yTopCtb*picWidthInCtbs] : -1;
  int topleftCTBTileID = availableTopLeft ? pps->TileIdRS[xLeftCtb+yTopCtb*picWidthInCtbs] : -1;
  int toprightCTBTileID= availableTopRight? pps->TileIdRS[xRightCtb+yTopCtb*picWidthInCtbs] : -1;

  if (leftCTBSlice != currCTBSlice  || leftCTBTileID != currCTBTileID ) availableLeft    = false;
  if (topCTBSlice  != currCTBSlice  || topCTBTileID  != currCTBTileID ) availableTop     = false;
  if (topleftCTBSlice !=currCTBSlice||topleftCTBTileID!=currCTBTileID ) availableTopLeft = false;
  if (toprightCTBSlice!=currCTBSlice||toprightCTBTileID!=currCTBTileID) availableTopRight= false;

  int currBlockAddr = pps->MinTbAddrZS[ (xBLuma>>sps->Log2MinTrafoSize) +
                                        (yBLuma>>sps->Log2MinTrafoSize) * sps->PicWidthInTbsY ];


  // number of pixels that are in the valid image area to the right and to the bottom

  int nBottom = sps->pic_height_in_luma_samples - (cIdx==0 ? yB : 2*yB);
  if (cIdx) nBottom=(nBottom+1)/2;
  if (nBottom>2*nT) nBottom=2*nT;
  int nRight  = sps->pic_width_in_luma_samples  - (cIdx==0 ? xB : 2*xB);
  if (cIdx) nRight =(nRight +1)/2;
  if (nRight >2*nT) nRight=2*nT;

  int nAvail=0;

  uint8_t firstValue;

  memset(available-2*nT, 0, 4*nT+1);

  {
    // copy pixels at left column

    for (int y=nBottom-1 ; y>=0 ; y-=4)
      if (availableLeft)
        {
          int NBlockAddr = pps->MinTbAddrZS[ ((xB-1)>>TUShift) +
                                             ((yB+y)>>TUShift) * sps->PicWidthInTbsY ];
        
          bool availableN = NBlockAddr < currBlockAddr;

          if (pps->constrained_intra_pred_flag) {
            if (img->get_pred_mode((xB-1)<<chromaShift,(yB+y)<<chromaShift)!=MODE_INTRA)
              availableN = false;
          }

          if (availableN) {
            if (!nAvail) firstValue = image[xB-1 + (yB+y)*stride];

            for (int i=0;i<4;i++) {
              available[-y+i-1] = availableN;
              out_border[-y+i-1] = image[xB-1 + (yB+y-i)*stride];
            }

            nAvail+=4;
          }
        }

    // copy pixel at top-left position

    if (availableTopLeft)
      {
        int NBlockAddr = pps->MinTbAddrZS[ ((xB-1)>>TUShift) +
                                           ((yB-1)>>TUShift) * sps->PicWidthInTbsY ];

        bool availableN = NBlockAddr < currBlockAddr;

        if (pps->constrained_intra_pred_flag) {
          if (img->get_pred_mode((xB-1)<<chromaShift,(yB-1)<<chromaShift)!=MODE_INTRA) {
            availableN = false;
          }
        }

        if (availableN) {
          if (!nAvail) firstValue = image[xB-1 + (yB-1)*stride];

          out_border[0] = image[xB-1 + (yB-1)*stride];
          available[0] = availableN;
          nAvail++;
        }
      }

    // copy pixels at top row

    for (int x=0 ; x<nRight ; x+=4) {
      bool borderAvailable;
      if (x<nT) borderAvailable=availableTop;
      else      borderAvailable=availableTopRight;

      if (borderAvailable)
        {
          int NBlockAddr = pps->MinTbAddrZS[ ((xB+x)>>TUShift) +
                                             ((yB-1)>>TUShift) * sps->PicWidthInTbsY ];

          bool availableN = NBlockAddr < currBlockAddr;

          if (pps->constrained_intra_pred_flag) {
            if (img->get_pred_mode((xB+x)<<chromaShift,(yB-1)<<chromaShift)!=MODE_INTRA) {
              availableN = false;
            }
          }


          if (availableN) {
            if (!nAvail) firstValue = image[xB+x + (yB-1)*stride];

            for (int i=0;i<4;i++) {
              out_border[x+i+1] = image[xB+x+i + (yB-1)*stride];
              available[x+i+1] = availableN;
            }

            nAvail+=4;
          }
        }
    }


    // reference sample substitution

    if (nAvail!=4*nT+1) {
      if (nAvail==0) {
        memset(out_border-2*nT, 1<<(sps->bit_depth_luma-1), 4*nT+1);
      }
      else {
        if (!available[-2*nT]) {
          out_border[-2*nT] = firstValue;
        }

        for (int i=-2*nT+1; i<=2*nT; i++)
          if (!available[i]) {
            out_border[i]=out_border[i-1];
          }
      }
    }

    logtrace(LogIntraPred,"availableN: ");
    print_border(available,NULL,nT);
    logtrace(LogIntraPred,"\n");

    logtrace(LogIntraPred,"output:     ");
    print_border(out_border,NULL,nT);
    logtrace(LogIntraPred,"\n");
  }
}


// (8.4.4.2.3)
void intra_prediction_sample_filtering(de265_image* img,
                                       uint8_t* p,
                                       int nT,
                                       enum IntraPredMode intraPredMode)
{
  int filterFlag;

  if (intraPredMode==INTRA_DC || nT==4) {
    filterFlag = 0;
  } else {
    // int-cast below prevents a typing problem that leads to wrong results when abs_value is a macro
    int minDistVerHor = libde265_min( abs_value((int)intraPredMode-26),
                                      abs_value((int)intraPredMode-10) );
    switch (nT) {
    case 8:  filterFlag = (minDistVerHor>7) ? 1 : 0; break;
    case 16: filterFlag = (minDistVerHor>1) ? 1 : 0; break;
    case 32: filterFlag = (minDistVerHor>0) ? 1 : 0; break;
    default: filterFlag = -1; assert(false); break; // should never happen
    }
  }


  if (filterFlag) {
    int biIntFlag = (img->sps.strong_intra_smoothing_enable_flag &&
                     nT==32 &&
                     abs_value(p[0]+p[ 64]-2*p[ 32]) < (1<<(img->sps.bit_depth_luma-5)) &&
                     abs_value(p[0]+p[-64]-2*p[-32]) < (1<<(img->sps.bit_depth_luma-5)))
      ? 1 : 0;

    uint8_t  pF_mem[2*64+1];
    uint8_t* pF = &pF_mem[64];

    if (biIntFlag) {
      pF[-2*nT] = p[-2*nT];
      pF[ 2*nT] = p[ 2*nT];
      pF[    0] = p[    0];

      for (int i=1;i<=63;i++) {
        pF[-i] = p[0] + ((i*(p[-64]-p[0])+32)>>6);
        pF[ i] = p[0] + ((i*(p[ 64]-p[0])+32)>>6);
      }
    } else {
      pF[-2*nT] = p[-2*nT];
      pF[ 2*nT] = p[ 2*nT];

      for (int i=-(2*nT-1) ; i<=2*nT-1 ; i++)
        {
          pF[i] = (p[i+1] + 2*p[i] + p[i-1] + 2) >> 2;
        }
    }


    // copy back to original array

    memcpy(p-2*nT, pF-2*nT, 4*nT+1);
  }
  else {
    // do nothing ?
  }


  logtrace(LogIntraPred,"post filtering: ");
  print_border(p,NULL,nT);
  logtrace(LogIntraPred,"\n");
}


const int intraPredAngle_table[1+34] =
  { 0, 0,32,26,21,17,13, 9, 5, 2, 0,-2,-5,-9,-13,-17,-21,-26,
    -32,-26,-21,-17,-13,-9,-5,-2,0,2,5,9,13,17,21,26,32 };

static const int invAngle_table[25-10] =
  { -4096,-1638,-910,-630,-482,-390,-315,-256,
    -315,-390,-482,-630,-910,-1638,-4096 };


// TODO: clip to read BitDepthY
LIBDE265_INLINE static int Clip1Y(int x) { if (x<0) return 0; else if (x>255) return 255; else return x; }


// (8.4.4.2.6)
void intra_prediction_angular(de265_image* img,
                              int xB0,int yB0,
                              enum IntraPredMode intraPredMode,
                              int nT,int cIdx,
                              uint8_t* border)
{
  uint8_t  ref_mem[2*64+1];
  uint8_t* ref=&ref_mem[64];

  uint8_t* pred;
  int      stride;
  pred   = img->get_image_plane_at_pos(cIdx,xB0,yB0);
  stride = img->get_image_stride(cIdx);

  int intraPredAngle = intraPredAngle_table[intraPredMode];

  if (intraPredMode >= 18) {

    for (int x=0;x<=nT;x++)
      { ref[x] = border[x]; }

    if (intraPredAngle<0) {
      int invAngle = invAngle_table[intraPredMode-11];

      if ((nT*intraPredAngle)>>5 < -1) {
        for (int x=(nT*intraPredAngle)>>5; x<=-1; x++) {
          ref[x] = border[0-((x*invAngle+128)>>8)];
        }
      }
    } else {
      for (int x=nT+1; x<=2*nT;x++) {
        ref[x] = border[x];
      }
    }

    for (int y=0;y<nT;y++)
      for (int x=0;x<nT;x++)
        {
          int iIdx = ((y+1)*intraPredAngle)>>5;
          int iFact= ((y+1)*intraPredAngle)&31;

          if (iFact != 0) {
            pred[x+y*stride] = ((32-iFact)*ref[x+iIdx+1] + iFact*ref[x+iIdx+2] + 16)>>5;
          } else {
            pred[x+y*stride] = ref[x+iIdx+1];
          }
        }

    if (intraPredMode==26 && cIdx==0 && nT<32) {
      for (int y=0;y<nT;y++) {
        pred[0+y*stride] = Clip1Y(border[1] + ((border[-1-y] - border[0])>>1));
      }
    }
  }
  else { // intraPredAngle < 18

    for (int x=0;x<=nT;x++)
      { ref[x] = border[-x]; }  // DIFF (neg)

    if (intraPredAngle<0) {
      int invAngle = invAngle_table[intraPredMode-11];

      if ((nT*intraPredAngle)>>5 < -1) {
        for (int x=(nT*intraPredAngle)>>5; x<=-1; x++) {
          ref[x] = border[((x*invAngle+128)>>8)]; // DIFF (neg)
        }
      }
    } else {
      for (int x=nT+1; x<=2*nT;x++) {
        ref[x] = border[-x]; // DIFF (neg)
      }
    }

    for (int y=0;y<nT;y++)
      for (int x=0;x<nT;x++)
        {
          int iIdx = ((x+1)*intraPredAngle)>>5;  // DIFF (x<->y)
          int iFact= ((x+1)*intraPredAngle)&31;  // DIFF (x<->y)

          if (iFact != 0) {
            pred[x+y*stride] = ((32-iFact)*ref[y+iIdx+1] + iFact*ref[y+iIdx+2] + 16)>>5; // DIFF (x<->y)
          } else {
            pred[x+y*stride] = ref[y+iIdx+1]; // DIFF (x<->y)
          }
        }

    if (intraPredMode==10 && cIdx==0 && nT<32) {  // DIFF 26->10
      for (int x=0;x<nT;x++) { // DIFF (x<->y)
        pred[x] = Clip1Y(border[-1] + ((border[1+x] - border[0])>>1)); // DIFF (x<->y && neg)
      }
    }
  }


  logtrace(LogIntraPred,"result of angular intra prediction (mode=%d):\n",intraPredMode);

  for (int y=0;y<nT;y++)
    {
      for (int x=0;x<nT;x++)
        logtrace(LogIntraPred,"%02x ", pred[x+y*stride]);

      logtrace(LogIntraPred,"\n");
    }
}


void intra_prediction_planar(de265_image* img,int xB0,int yB0,int nT,int cIdx,
                             uint8_t* border)
{
  uint8_t* pred;
  int      stride;
  pred = img->get_image_plane_at_pos(cIdx,xB0,yB0);
  stride = img->get_image_stride(cIdx);

  int Log2_nT = Log2(nT);

  for (int y=0;y<nT;y++)
    for (int x=0;x<nT;x++)
      {
        pred[x+y*stride] = ((nT-1-x)*border[-1-y] + (x+1)*border[ 1+nT] +
                            (nT-1-y)*border[ 1+x] + (y+1)*border[-1-nT] + nT) >> (Log2_nT+1);
      }


  logtrace(LogIntraPred,"result of planar prediction\n");

  for (int y=0;y<nT;y++)
    {
      for (int x=0;x<nT;x++)
        logtrace(LogIntraPred,"%02x ", pred[x+y*stride]);

      logtrace(LogIntraPred,"\n");
    }
}


void intra_prediction_DC(de265_image* img,int xB0,int yB0,int nT,int cIdx,
                         uint8_t* border)
{
  uint8_t* pred;
  int      stride;
  pred = img->get_image_plane_at_pos(cIdx,xB0,yB0);
  stride = img->get_image_stride(cIdx);

  int Log2_nT = Log2(nT);

  int dcVal = 0;
  for (int i=0;i<nT;i++)
    {
      dcVal += border[ i+1];
      dcVal += border[-i-1];
    }

  dcVal += nT;
  dcVal >>= Log2_nT+1;

  if (cIdx==0 && nT<32) {
    pred[0] = (border[-1] + 2*dcVal + border[1] +2) >> 2;

    for (int x=1;x<nT;x++) { pred[x]        = (border[ x+1] + 3*dcVal+2)>>2; }
    for (int y=1;y<nT;y++) { pred[y*stride] = (border[-y-1] + 3*dcVal+2)>>2; }
    for (int y=1;y<nT;y++)
      for (int x=1;x<nT;x++)
        {
          pred[x+y*stride] = dcVal;
        }
  } else {
    for (int y=0;y<nT;y++)
      for (int x=0;x<nT;x++)
        {
          pred[x+y*stride] = dcVal;
        }
  }


  logtrace(LogIntraPred,"INTRAPRED DC\n");
  for (int y=0;y<nT;y++)
    {
      for (int x=0;x<nT;x++)
        logtrace(LogIntraPred,"%02x ", pred[x+y*stride]);

      logtrace(LogIntraPred,"\n");
    }
}



// (8.4.4.2.1)
void decode_intra_prediction(de265_image* img,
                             int xB0,int yB0,
                             enum IntraPredMode intraPredMode,
                             int nT, int cIdx)
{
  logtrace(LogIntraPred,"decode_intra_prediction xy0:%d/%d mode=%d nT=%d, cIdx=%d\n",
           xB0,yB0, intraPredMode, nT,cIdx);
  /*
    printf("decode_intra_prediction xy0:%d/%d mode=%d nT=%d, cIdx=%d\n",
    xB0,yB0, intraPredMode, nT,cIdx);
  */

  nIntraPredictions++;

  uint8_t  border_pixels_mem[2*64+1];
  uint8_t* border_pixels = &border_pixels_mem[64];

  fill_border_samples(img, xB0,yB0, nT, cIdx, border_pixels);

  if (cIdx==0) {
    intra_prediction_sample_filtering(img, border_pixels, nT, intraPredMode);
  }


  switch (intraPredMode) {
  case INTRA_PLANAR:
    intra_prediction_planar(img,xB0,yB0,nT,cIdx, border_pixels);
    break;
  case INTRA_DC:
    intra_prediction_DC(img,xB0,yB0,nT,cIdx, border_pixels);
    break;
  default:
    intra_prediction_angular(img,xB0,yB0,intraPredMode,nT,cIdx, border_pixels);
    break;
  }
}


