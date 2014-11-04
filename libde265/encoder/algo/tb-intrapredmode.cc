/*
 * H.265 video codec.
 * Copyright (c) 2013-2014 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * Authors: struktur AG, Dirk Farin <farin@struktur.de>
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


#include "libde265/encoder/analyze.h"
#include "libde265/encoder/encoder-context.h"
#include "libde265/encoder/algo/tb-split.h"
#include <assert.h>
#include <limits>
#include <math.h>


#define ENCODER_DEVELOPMENT 1



enum IntraPredMode find_best_intra_mode(de265_image& img,int x0,int y0, int log2BlkSize, int cIdx,
                                        const uint8_t* ref, int stride)
{
  //return INTRA_ANGULAR_20;

  enum IntraPredMode best_mode;
  int min_sad=-1;

  int candidates[3];

  const seq_parameter_set* sps = &img.sps;


  fillIntraPredModeCandidates(candidates, x0,y0,
                              sps->getPUIndexRS(x0,y0),
                              x0>0, y0>0, &img);

  // --- test candidates first ---

  for (int idx=0;idx<3;idx++) {
    enum IntraPredMode mode = (enum IntraPredMode)candidates[idx];
    decode_intra_prediction(&img, x0,y0, (enum IntraPredMode)mode, 1<<log2BlkSize, cIdx);

    uint32_t distortion = SSD(ref,stride,
      img.get_image_plane_at_pos(cIdx, x0,y0), img.get_image_stride(cIdx),
      1<<log2BlkSize, 1<<log2BlkSize);

    int sad=distortion;

    sad *= 0.5;
    //sad *= 0.9;

    if (mode==0 || sad<min_sad) {
      min_sad = sad;
      best_mode = (enum IntraPredMode)mode;
    }
  }


  // --- test all modes ---

  for (int idx=0;idx<35;idx++) {
    enum IntraPredMode mode = (enum IntraPredMode)idx; //candidates[idx];
    decode_intra_prediction(&img, x0,y0, (enum IntraPredMode)mode, 1<<log2BlkSize, cIdx);


    uint32_t distortion = SSD(ref,stride,
      img.get_image_plane_at_pos(cIdx, x0,y0), img.get_image_stride(cIdx),
      1<<log2BlkSize, 1<<log2BlkSize);

    int sad=distortion;

    if (min_sad<0 || sad<min_sad) {
      min_sad = sad;
      best_mode = (enum IntraPredMode)mode;
    }
  }

  return best_mode;
}


static void diff_blk(int16_t* out,int out_stride,
                     const uint8_t* a_ptr, int a_stride,
                     const uint8_t* b_ptr, int b_stride,
                     int blkSize)
{
  for (int by=0;by<blkSize;by++)
    for (int bx=0;bx<blkSize;bx++)
      {
        out[by*out_stride+bx] = a_ptr[by*a_stride+bx] - b_ptr[by*b_stride+bx];
      }
}



float estim_TB_bitrate(const encoder_context* ectx,
                       const de265_image* input,
                       int x0,int y0, int log2BlkSize,
                       enum TBBitrateEstimMethod method)
{
  int blkSize = 1<<log2BlkSize;

  float distortion;

  switch (method)
    {
    case TBBitrateEstim_SSD:
      return SSD(input->get_image_plane_at_pos(0, x0,y0),
                 input->get_image_stride(0),
                 ectx->img->get_image_plane_at_pos(0, x0,y0),
                 ectx->img->get_image_stride(0),
                 1<<log2BlkSize, 1<<log2BlkSize);
      break;

    case TBBitrateEstim_SAD:
      return SAD(input->get_image_plane_at_pos(0, x0,y0),
                 input->get_image_stride(0),
                 ectx->img->get_image_plane_at_pos(0, x0,y0),
                 ectx->img->get_image_stride(0),
                 1<<log2BlkSize, 1<<log2BlkSize);
      break;

    case TBBitrateEstim_SATD_DCT:
    case TBBitrateEstim_SATD_Hadamard:
      {
        int16_t coeffs[32*32];
        int16_t diff[32*32];

        diff_blk(diff,blkSize,
                 input->get_image_plane_at_pos(0, x0,y0), input->get_image_stride(0),
                 ectx->img->get_image_plane_at_pos(0, x0,y0), ectx->img->get_image_stride(0),
                 blkSize);

        if (method == TBBitrateEstim_SATD_Hadamard) {
          ectx->accel.hadamard_transform_8[log2BlkSize-2](coeffs, diff, &diff[blkSize] - &diff[0]);
        }
        else {
          ectx->accel.fwd_transform_8[log2BlkSize-2](coeffs, diff, &diff[blkSize] - &diff[0]);
        }

        float distortion=0;
        for (int i=0;i<blkSize*blkSize;i++) {
          distortion += abs_value((int)coeffs[i]);
        }

        return distortion;
      }
      break;

      /*
    case TBBitrateEstim_AccurateBits:
      assert(false);
      return 0;
      */
    }

  assert(false);
  return 0;
}



const enc_tb*
Algo_TB_IntraPredMode_BruteForce::analyze(encoder_context* ectx,
                                          context_model_table ctxModel,
                                          const de265_image* input,
                                          const enc_tb* parent,
                                          enc_cb* cb,
                                          int x0,int y0, int xBase,int yBase,
                                          int log2TbSize, int blkIdx,
                                          int TrafoDepth, int MaxTrafoDepth,
                                          int IntraSplitFlag)
{
  //printf("encode_transform_tree_may_split %d %d (%d %d) size %d\n",x0,y0,xBase,yBase,1<<log2TbSize);

  /*
    enum IntraPredMode pre_intraMode = find_best_intra_mode(ectx->img,x0,y0, log2TbSize, 0,
    input->get_image_plane_at_pos(0,x0,y0),
    input->get_image_stride(0));
  */

  bool selectIntraPredMode = false;
  selectIntraPredMode |= (cb->PredMode==MODE_INTRA && cb->PartMode==PART_2Nx2N && TrafoDepth==0);
  selectIntraPredMode |= (cb->PredMode==MODE_INTRA && cb->PartMode==PART_NxN   && TrafoDepth==1);

  if (selectIntraPredMode) {
    const enc_tb* tb[35];

    float minCost = std::numeric_limits<float>::max();
    int   minCostIdx=0;
    float minCandCost;

    const de265_image* img = ectx->img;
    const seq_parameter_set* sps = &img->sps;
    int candidates[3];
    fillIntraPredModeCandidates(candidates, x0,y0,
                                sps->getPUIndexRS(x0,y0),
                                x0>0, y0>0, img);


    for (int i = 0; i<35; i++) {
      if (!mPredMode_enabled[i]) {
        tb[i]=NULL;
        continue;
      }


      context_model_table ctxIntra;
      copy_context_model_table(ctxIntra, ctxModel);

      enum IntraPredMode intraMode = (IntraPredMode)i;

      cb->intra.pred_mode[blkIdx] = intraMode;
      if (blkIdx==0) { cb->intra.chroma_mode = intraMode; }

      ectx->img->set_IntraPredMode(x0,y0,log2TbSize, intraMode);

      tb[intraMode] = mTBSplitAlgo->analyze(ectx,ctxIntra,input,parent,
                                            cb, x0,y0, xBase,yBase, log2TbSize, blkIdx,
                                            TrafoDepth, MaxTrafoDepth, IntraSplitFlag);


      float sad;
      if ((1<<log2TbSize)==8) {
        decode_intra_prediction(ectx->img, x0,y0, intraMode, 1<<log2TbSize, 0);
        sad = estim_TB_bitrate(ectx,input, x0,y0, log2TbSize, TBBitrateEstim_SAD);
      }


      float rate = tb[intraMode]->rate;
      int enc_bin;

      if (log2TbSize==3) {
        // printf("RATE2 %d %f %f\n",log2TbSize,tb[intraMode]->rate,sad);
      }

      /**/ if (candidates[0]==intraMode) { rate += 1; enc_bin=1; }
      else if (candidates[1]==intraMode) { rate += 2; enc_bin=1; }
      else if (candidates[2]==intraMode) { rate += 2; enc_bin=1; }
      else { rate += 5; enc_bin=0; }

      rate += CABAC_encoder::RDBits_for_CABAC_bin(&ctxIntra[CONTEXT_MODEL_PREV_INTRA_LUMA_PRED_FLAG], enc_bin);

      float cost = tb[intraMode]->distortion + ectx->lambda * rate;
      if (cost<minCost) {
        minCost=cost;
        minCostIdx=intraMode;
        //minCandCost=c;
      }
    }


    enum IntraPredMode intraMode = (IntraPredMode)minCostIdx;

    cb->intra.pred_mode[blkIdx] = intraMode;
    if (blkIdx==0) { cb->intra.chroma_mode  = intraMode; } //INTRA_CHROMA_LIKE_LUMA;
    ectx->img->set_IntraPredMode(x0,y0,log2TbSize, intraMode);

    tb[minCostIdx]->reconstruct(&ectx->accel,
                                ectx->img, x0,y0, xBase,yBase,
                                cb, blkIdx);


    //printf("INTRA %d %d  %d\n",pre_intraMode,intraMode,minCandCost);

    for (int i = 0; i<35; i++) {
      if (i != minCostIdx) {
        delete tb[i];
      }
    }

    return tb[minCostIdx];
  }
  else {
    return mTBSplitAlgo->analyze(ectx, ctxModel, input, parent, cb,
                                 x0,y0,xBase,yBase, log2TbSize,
                                 blkIdx, TrafoDepth, MaxTrafoDepth,
                                 IntraSplitFlag);
  }
}



const enc_tb*
Algo_TB_IntraPredMode_MinResidual::analyze(encoder_context* ectx,
                                           context_model_table ctxModel,
                                           const de265_image* input,
                                           const enc_tb* parent,
                                           enc_cb* cb,
                                           int x0,int y0, int xBase,int yBase,
                                           int log2TbSize, int blkIdx,
                                           int TrafoDepth, int MaxTrafoDepth,
                                           int IntraSplitFlag)
{

  bool selectIntraPredMode = false;
  selectIntraPredMode |= (cb->PredMode==MODE_INTRA && cb->PartMode==PART_2Nx2N && TrafoDepth==0);
  selectIntraPredMode |= (cb->PredMode==MODE_INTRA && cb->PartMode==PART_NxN   && TrafoDepth==1);

  if (selectIntraPredMode) {

    enum IntraPredMode intraMode;
    float minDistortion;

    for (int idx=0;idx<35;idx++) {
      enum IntraPredMode mode = (enum IntraPredMode)idx;
      decode_intra_prediction(ectx->img, x0,y0, (enum IntraPredMode)mode, 1<<log2TbSize, 0);

      float distortion;
      distortion = estim_TB_bitrate(ectx, input, x0,y0, log2TbSize,
                                    mParams.bitrateEstimMethod());

      if (idx==0 || distortion<minDistortion) {
        minDistortion = distortion;
        intraMode = mode;
      }
    }

    //intraMode=(enum IntraPredMode)(rand()%35);


    cb->intra.pred_mode[blkIdx] = intraMode;
    if (blkIdx==0) { cb->intra.chroma_mode = intraMode; }

    ectx->img->set_IntraPredMode(x0,y0,log2TbSize, intraMode);

    const enc_tb* tb = mTBSplitAlgo->analyze(ectx,ctxModel,input,parent,
                                             cb, x0,y0, xBase,yBase, log2TbSize, blkIdx,
                                             TrafoDepth, MaxTrafoDepth, IntraSplitFlag);

    tb->reconstruct(&ectx->accel,
                    ectx->img, x0,y0, xBase,yBase,
                    cb, blkIdx);

    return tb;
  }
  else {
    return mTBSplitAlgo->analyze(ectx, ctxModel, input, parent, cb,
                                 x0,y0,xBase,yBase, log2TbSize,
                                 blkIdx, TrafoDepth, MaxTrafoDepth,
                                 IntraSplitFlag);
  }
}

#include <algorithm>
static bool sortDistortions(std::pair<enum IntraPredMode,float> i,
                            std::pair<enum IntraPredMode,float> j)
{
  return i.second < j.second;
}

const enc_tb*
Algo_TB_IntraPredMode_FastBrute::analyze(encoder_context* ectx,
                                         context_model_table ctxModel,
                                         const de265_image* input,
                                         const enc_tb* parent,
                                         enc_cb* cb,
                                         int x0,int y0, int xBase,int yBase,
                                         int log2TbSize, int blkIdx,
                                         int TrafoDepth, int MaxTrafoDepth,
                                         int IntraSplitFlag)
{
  //printf("encode_transform_tree_may_split %d %d (%d %d) size %d\n",x0,y0,xBase,yBase,1<<log2TbSize);

  /*
    enum IntraPredMode pre_intraMode = find_best_intra_mode(ectx->img,x0,y0, log2TbSize, 0,
    input->get_image_plane_at_pos(0,x0,y0),
    input->get_image_stride(0));
  */

  bool selectIntraPredMode = false;
  selectIntraPredMode |= (cb->PredMode==MODE_INTRA && cb->PartMode==PART_2Nx2N && TrafoDepth==0);
  selectIntraPredMode |= (cb->PredMode==MODE_INTRA && cb->PartMode==PART_NxN   && TrafoDepth==1);

  if (selectIntraPredMode) {
    float minCost = std::numeric_limits<float>::max();
    int   minCostIdx=0;
    float minCandCost;

    const de265_image* img = ectx->img;
    const seq_parameter_set* sps = &img->sps;
    int candidates[3];
    fillIntraPredModeCandidates(candidates, x0,y0,
                                sps->getPUIndexRS(x0,y0),
                                x0>0, y0>0, img);



    std::vector< std::pair<enum IntraPredMode,float> > distortions;

    for (int idx=0;idx<35;idx++)
      if (idx!=candidates[0] && idx!=candidates[1] && idx!=candidates[2] && mPredMode_enabled[idx])
        {
          enum IntraPredMode mode = (enum IntraPredMode)idx;
          decode_intra_prediction(ectx->img, x0,y0, (enum IntraPredMode)mode, 1<<log2TbSize, 0);
          
          float distortion;
          distortion = estim_TB_bitrate(ectx, input, x0,y0, log2TbSize,
                                        mParams.bitrateEstimMethod());

          distortions.push_back( std::make_pair((enum IntraPredMode)idx, distortion) );
        }

    std::sort( distortions.begin(), distortions.end(), sortDistortions );


    for (int i=0;i<distortions.size();i++)
      {
        //printf("%d -> %f\n",i,distortions[i].second);
      }

    int keepNBest=mParams.keepNBest;
    distortions.resize(keepNBest);
    distortions.push_back(std::make_pair((enum IntraPredMode)candidates[0],0));
    distortions.push_back(std::make_pair((enum IntraPredMode)candidates[1],0));
    distortions.push_back(std::make_pair((enum IntraPredMode)candidates[2],0));


    const enc_tb* tb[35];

    for (int i=0;i<35;i++) tb[i]=NULL;


    for (int i=0;i<distortions.size();i++) {

      context_model_table ctxIntra;
      copy_context_model_table(ctxIntra, ctxModel);

      enum IntraPredMode intraMode = (IntraPredMode)distortions[i].first;

      cb->intra.pred_mode[blkIdx] = intraMode;
      if (blkIdx==0) { cb->intra.chroma_mode = intraMode; }

      ectx->img->set_IntraPredMode(x0,y0,log2TbSize, intraMode);

      tb[intraMode] = mTBSplitAlgo->analyze(ectx,ctxIntra,input,parent,
                                            cb, x0,y0, xBase,yBase, log2TbSize, blkIdx,
                                            TrafoDepth, MaxTrafoDepth, IntraSplitFlag);


      float rate = tb[intraMode]->rate;
      int enc_bin;

      /**/ if (candidates[0]==intraMode) { rate += 1; enc_bin=1; }
      else if (candidates[1]==intraMode) { rate += 2; enc_bin=1; }
      else if (candidates[2]==intraMode) { rate += 2; enc_bin=1; }
      else { rate += 5; enc_bin=0; }

      rate += CABAC_encoder::RDBits_for_CABAC_bin(&ctxIntra[CONTEXT_MODEL_PREV_INTRA_LUMA_PRED_FLAG], enc_bin);

      float cost = tb[intraMode]->distortion + ectx->lambda * rate;
      if (cost<minCost) {
        minCost=cost;
        minCostIdx=intraMode;
        //minCandCost=c;
      }
    }


    enum IntraPredMode intraMode = (IntraPredMode)minCostIdx;

    cb->intra.pred_mode[blkIdx] = intraMode;
    if (blkIdx==0) { cb->intra.chroma_mode  = intraMode; } //INTRA_CHROMA_LIKE_LUMA;
    ectx->img->set_IntraPredMode(x0,y0,log2TbSize, intraMode);

    tb[minCostIdx]->reconstruct(&ectx->accel,
                                ectx->img, x0,y0, xBase,yBase,
                                cb, blkIdx);

    for (int i = 0; i<35; i++) {
      if (i != minCostIdx) {
        delete tb[i];
      }
    }

    //printf("INTRA %d %d  %d\n",pre_intraMode,intraMode,minCandCost);

    return tb[minCostIdx];
  }
  else {
    return mTBSplitAlgo->analyze(ectx, ctxModel, input, parent, cb,
                                 x0,y0,xBase,yBase, log2TbSize,
                                 blkIdx, TrafoDepth, MaxTrafoDepth,
                                 IntraSplitFlag);
  }
}

