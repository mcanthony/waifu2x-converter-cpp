/* -*- mode: c -*- */

__kernel void
filter(__global const float * __restrict__ packed_input,
       unsigned int nInputPlanes,
       __global float * __restrict__ packed_output,
       unsigned int nOutputPlanes,
       __global float * __restrict__ biases,
       unsigned int hsz,
       unsigned int wsz,
       __global float * __restrict__ weight,
       __local float * __restrict__ local_mem)
{
    unsigned int yi = get_group_id(0);
    unsigned int lid = get_local_id(0);

    __global const float * __restrict__ in = packed_input;
    size_t in_step = wsz * sizeof(float) * nInputPlanes;

    __global char *inp = (__global char*)packed_input;

    inp += in_step*yi;
    __global char *in0p = inp - in_step;
    if (yi == 0) {
        in0p = inp;
    }

    __global char *in1p = inp;
    __global char *in2p = inp + in_step;

    if (yi == hsz-1) {
        in2p = inp;
    }

    unsigned int vec_width = min((int)VEC_WIDTH, (int)nOutputPlanes);
    unsigned int nOutputBlock = nOutputPlanes / vec_width;
    //int inputBlockSize = nInputPlanes/2;
    //unsigned int nInputBlock = (nInputPlanes+1U)/2U;
    //unsigned int nInputBlock = 2;

    //unsigned int inputBlockSize = 4U;
    //unsigned int nInputBlock = (nInputPlanes+(inputBlockSize-1))/inputBlockSize;
    unsigned int inputBlockSize = nInputPlanes;
    unsigned int nInputBlock = 1;

    /* local_size = 16KB - arguments = 14KB?
     *
     * weight size = output block size(vec_width) * input block_size * 9 * sizeof(float)
     *  vec_width = 64,
     *  input block_size = 4
     *  64*4*9*4 = 9216 byte
     *
     * intermediate size = output block size * x block size * sizeof(float)
     *
     */

    //__local float *weight_local = (__local float*)local_mem;
    //local_mem += VEC_WIDTH * inputBlockSize * 9;

    __local float *input_buffer0 = (__local float*)local_mem;
    local_mem += nInputPlanes * 4;
    __local float *input_buffer1 = (__local float*)local_mem;
    local_mem += nInputPlanes * 4;
    __local float *input_buffer2 = (__local float*)local_mem;
    local_mem += nInputPlanes * 4;

    unsigned int x_block_size = 16U;
    unsigned int x_block_count = (wsz+(x_block_size-1))/x_block_size;

    //for (int ibi=0; ibi<nInputBlock; ibi++)
    {
        unsigned int ibi = 0;
        unsigned int ipBegin = ibi * inputBlockSize;
        unsigned int ipEnd = min(ipBegin + inputBlockSize, nInputPlanes);

        for (int obi=0; obi<nOutputBlock; obi++) {
            __global float *out = packed_output + (yi*wsz)*nOutputPlanes;

#if 0
            __local float *weight_local_ptr = weight_local + lid;
            __global float *w = weight + lid + (ipBegin*nOutputPlanes + obi*vec_width)*9;

            int nip = ipEnd - ipBegin;
            for (unsigned int ipIndex = 0; ipIndex<nip; ipIndex++) {
                weight_local_ptr[0*VEC_WIDTH] = w[0*vec_width];
                weight_local_ptr[1*VEC_WIDTH] = w[1*vec_width];
                weight_local_ptr[2*VEC_WIDTH] = w[2*vec_width];

                weight_local_ptr[3*VEC_WIDTH] = w[3*vec_width];
                weight_local_ptr[4*VEC_WIDTH] = w[4*vec_width];
                weight_local_ptr[5*VEC_WIDTH] = w[5*vec_width];

                weight_local_ptr[6*VEC_WIDTH] = w[6*vec_width];
                weight_local_ptr[7*VEC_WIDTH] = w[7*vec_width];
                weight_local_ptr[8*VEC_WIDTH] = w[8*vec_width];

                w += 9 * nOutputPlanes;
                weight_local_ptr += 9*VEC_WIDTH;
            }
#endif

            for (int xi=0; xi<wsz; xi+=2) {
                float intermediate0 = 0;
                float intermediate1 = 0;

                __global float *in01 = (__global float*)in0p;
                __global float *in11 = (__global float*)in1p;
                __global float *in21 = (__global float*)in2p;

                in01 += xi * nInputPlanes + ipBegin;
                in11 += xi * nInputPlanes + ipBegin;
                in21 += xi * nInputPlanes + ipBegin;

                barrier(CLK_LOCAL_MEM_FENCE);

                for (unsigned int ipIndex = lid;
                     ipIndex < nInputPlanes;
                     ipIndex+=vec_width)
                {
                    float v01 = input_buffer0[ipIndex + nInputPlanes] = in01[ipIndex];
                    float v11 = input_buffer1[ipIndex + nInputPlanes] = in11[ipIndex];
                    float v21 = input_buffer2[ipIndex + nInputPlanes] = in21[ipIndex];
                    float v02 = input_buffer0[ipIndex + nInputPlanes*2] = in01[ipIndex + nInputPlanes*1];
                    float v12 = input_buffer1[ipIndex + nInputPlanes*2] = in11[ipIndex + nInputPlanes*1];
                    float v22 = input_buffer2[ipIndex + nInputPlanes*2] = in21[ipIndex + nInputPlanes*1];

                    if (xi == 0) {
                        input_buffer0[ipIndex] = v01;
                        input_buffer1[ipIndex] = v11;
                        input_buffer2[ipIndex] = v21;
                    } else {
                        input_buffer0[ipIndex] = in01[ipIndex - nInputPlanes];
                        input_buffer1[ipIndex] = in11[ipIndex - nInputPlanes];
                        input_buffer2[ipIndex] = in21[ipIndex - nInputPlanes];
                    }

                    if (xi+1 == wsz-1) {
                        input_buffer0[ipIndex + nInputPlanes*3] = v02;
                        input_buffer1[ipIndex + nInputPlanes*3] = v12;
                        input_buffer2[ipIndex + nInputPlanes*3] = v22;
                    } else {
                        input_buffer0[ipIndex + nInputPlanes*3] = in01[ipIndex + nInputPlanes*2];
                        input_buffer1[ipIndex + nInputPlanes*3] = in11[ipIndex + nInputPlanes*2];
                        input_buffer2[ipIndex + nInputPlanes*3] = in21[ipIndex + nInputPlanes*2];
                    }
                }

                barrier(CLK_LOCAL_MEM_FENCE);


                //__local float *w = weight_local + lid;
                __global float *w = weight + lid + (ipBegin*nOutputPlanes + obi*vec_width)*9;

                __local float *lin01 = input_buffer0 + nInputPlanes;
                __local float *lin11 = input_buffer1 + nInputPlanes;
                __local float *lin21 = input_buffer2 + nInputPlanes;

                for (unsigned int ipIndex = ipBegin;
                     ipIndex < ipEnd; ipIndex++)
                {
                    float i00, i01, i02, i03;
                    float i10, i11, i12, i13;
                    float i20, i21, i22, i23;

                    i01 = lin01[0];
                    i11 = lin11[0];
                    i21 = lin21[0];
                    i02 = lin01[+nInputPlanes];
                    i12 = lin11[+nInputPlanes];
                    i22 = lin21[+nInputPlanes];

                    if (xi == 0) {
                        i00 = i01;
                        i10 = i11;
                        i20 = i21;
                    } else {
                        i00 = lin01[-nInputPlanes];
                        i10 = lin11[-nInputPlanes];
                        i20 = lin21[-nInputPlanes];
                    }

                    if (xi+1 == wsz-1) {
                        i03 = i02;
                        i13 = i12;
                        i23 = i22;
                    } else {
                        i03 = lin01[+nInputPlanes*2];
                        i13 = lin11[+nInputPlanes*2];
                        i23 = lin21[+nInputPlanes*2];
                    }

                    lin01++;
                    lin11++;
                    lin21++;

                    float v0 = 0, v1 = 0;
                    int vw = vec_width;

                    v0 += w[0*vw] * i00;
                    v1 += w[0*vw] * i01;

                    v0 += w[1*vw] * i01;
                    v1 += w[1*vw] * i02;

                    v0 += w[2*vw] * i02;
                    v1 += w[2*vw] * i03;


                    v0 += w[3*vw] * i10;
                    v1 += w[3*vw] * i11;

                    v0 += w[4*vw] * i11;
                    v1 += w[4*vw] * i12;

                    v0 += w[5*vw] * i12;
                    v1 += w[5*vw] * i13;


                    v0 += w[6*vw] * i20;
                    v1 += w[6*vw] * i21;

                    v0 += w[7*vw] * i21;
                    v1 += w[7*vw] * i22;

                    v0 += w[8*vw] * i22;
                    v1 += w[8*vw] * i23;

                    //w += 9*vw;
                    w += 9*nOutputPlanes;

                    intermediate0 += v0;
                    intermediate1 += v1;
                }

                int opIndex = obi*vec_width + lid;

                if (ibi == nInputBlock-1) {
                    float bv = biases[opIndex];
                    float v, mtz, ltz;

                    v = intermediate0;

                    if (nInputBlock != 1) {
                        v += out[opIndex];
                    }
                    v += bv;
                    mtz = max(v, 0.0f);
                    ltz = min(v, 0.0f);
                    v = ltz * 0.1f + mtz;
                    out[opIndex] = v;
                    out += nOutputPlanes;


                    v = intermediate1;
                    if (nInputBlock != 1) {
                        v += out[opIndex];
                    }
                    v += bv;
                    mtz = max(v, 0.0f);
                    ltz = min(v, 0.0f);
                    v = ltz * 0.1f + mtz;
                    out[opIndex] = v;
                    out += nOutputPlanes;

                } else if (ibi == 0) {
                    out[opIndex] = intermediate0;
                    out += nOutputPlanes;
                    out[opIndex] = intermediate1;
                    out += nOutputPlanes;
                } else {
                    out[opIndex] += intermediate0;
                    out += nOutputPlanes;
                    out[opIndex] += intermediate1;
                    out += nOutputPlanes;
                }
            }
        }
    }
}

