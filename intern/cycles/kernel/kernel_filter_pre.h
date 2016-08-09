/*
 * Copyright 2011-2016 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

CCL_NAMESPACE_BEGIN

/* First step of the shadow prefiltering, performs the shadow division and stores all data
 * in a nice and easy rectangular array that can be passed to the NLM filter.
 *
 * Calculates:
 * unfiltered: Contains the two half images of the shadow feature pass
 * sampleVariance: The sample-based variance calculated in the kernel. Note: This calculation is biased in general, and especially here since the variance of the ratio can only be approximated.
 * sampleVarianceV: Variance of the sample variance estimation, quite noisy (since it's essentially the buffer variance of the two variance halves)
 * bufferVariance: The buffer-based variance of the shadow feature. Unbiased, but quite noisy.
 */
ccl_device void kernel_filter_divide_shadow(KernelGlobals *kg, int sample, float **buffers, int x, int y, int *tile_x, int *tile_y, int *offset, int *stride, float *unfiltered, float *sampleVariance, float *sampleVarianceV, float *bufferVariance, int4 rect)
{
	int xtile = (x < tile_x[1])? 0: ((x < tile_x[2])? 1: 2);
	int ytile = (y < tile_y[1])? 0: ((y < tile_y[2])? 1: 2);
	int tile = ytile*3+xtile;
	float *center_buffer = buffers[tile] + (offset[tile] + y*stride[tile] + x)*kernel_data.film.pass_stride + kernel_data.film.pass_denoising;

	int buffer_w = align_up(rect.z - rect.x, 4);
	int idx = (y-rect.y)*buffer_w + (x - rect.x);
	int Bofs = (rect.w - rect.y)*buffer_w;
	unfiltered[idx] = center_buffer[15] / max(center_buffer[14], 1e-7f);
	unfiltered[idx+Bofs] = center_buffer[18] / max(center_buffer[17], 1e-7f);
	float varFac = 1.0f / (sample * (sample-1));
	sampleVariance[idx] = (center_buffer[16] + center_buffer[19]) * varFac;
	sampleVarianceV[idx] = 0.5f * (center_buffer[16] - center_buffer[19]) * (center_buffer[16] - center_buffer[19]) * varFac;
	bufferVariance[idx] = 0.5f * (unfiltered[idx] - unfiltered[idx+Bofs]) * (unfiltered[idx] - unfiltered[idx+Bofs]);
}

/* Load a regular feature from the render buffers into the denoise buffer.
 * Parameters:
 * - sample: The sample amount in the buffer, used to normalize the buffer.
 * - buffers: 9-Element Array containing pointers to the buffers of the 3x3 tiles around the current one.
 * - m_offset, v_offset: Render Buffer Pass offsets of mean and variance of the feature.
 * - x, y: Current pixel
 * - tile_x, tile_y: 4-Element Arrays containing the x/y coordinates of the start of the lower, current and upper tile as well as the end of the upper tile plus one.
 * - offset, stride: 9-Element Arrays containing offset and stride of the RenderBuffers.
 * - mean, variance: Target denoise buffers.
 * - rect: The prefilter area (lower pixels inclusive, upper pixels exclusive).
 */
ccl_device void kernel_filter_get_feature(KernelGlobals *kg, int sample, float **buffers, int m_offset, int v_offset, int x, int y, int *tile_x, int *tile_y, int *offset, int *stride, float *mean, float *variance, int4 rect)
{
	int xtile = (x < tile_x[1])? 0: ((x < tile_x[2])? 1: 2);
	int ytile = (y < tile_y[1])? 0: ((y < tile_y[2])? 1: 2);
	int tile = ytile*3+xtile;
	float *center_buffer = buffers[tile] + (offset[tile] + y*stride[tile] + x)*kernel_data.film.pass_stride + kernel_data.film.pass_denoising;

	int buffer_w = align_up(rect.z - rect.x, 4);
	int idx = (y-rect.y)*buffer_w + (x - rect.x);
	mean[idx] = center_buffer[m_offset] / sample;
	variance[idx] = center_buffer[v_offset] / (sample * (sample-1));
}

/* Combine A/B buffers.
 * Calculates the combined mean and the buffer variance. */
ccl_device void kernel_filter_combine_halves(int x, int y, float *mean, float *variance, float *a, float *b, int4 rect)
{
	int buffer_w = align_up(rect.z - rect.x, 4);
	int idx = (y-rect.y)*buffer_w + (x - rect.x);

	if(mean)     mean[idx] = 0.5f * (a[idx]+b[idx]);
	if(variance) variance[idx] = 0.5f * (a[idx]-b[idx])*(a[idx]-b[idx]);
}

/* General Non-Local Means filter implementation.
 * NLM essentially is an extension of the bilaterail filter: It also loops over all the pixels in a neighborhood, calculates a weight for each one and combines them.
 * The difference is the weighting function: While the Bilateral filter just looks that the two pixels (center=p and pixel in neighborhood=q) and calculates the weight from
 * their distance and color difference, NLM considers small patches around both pixels and compares those. That way, it is able to identify similar image regions and compute
 * better weights.
 * One important consideration is that the image used for comparing patches doesn't have to be the one that's being filtered.
 * This is used in two different ways in the denoiser: First, by splitting the samples in half, we get two unbiased estimates of the image.
 * Then, we can use one of the halves to calculate the weights for filtering the other one. This way, the weights are decorrelated from the image and the result is smoother.
 * The second use is for variance: Sample variance (generated in the kernel) tends to be quite smooth, but is biased.
 * On the other hand, buffer variance, calculated from the difference of the two half images, is unbiased, but noisy.
 * Therefore, by filtering the buffer variance based on weights from the sample variance, we get the same smooth structure, but the unbiased result.

 * Parameters:
 * - x, y: The position that is to be filtered (=p in the algorithm)
 * - noisyImage: The image that is being filtered
 * - weightImage: The image used for comparing patches and calculating weights
 * - variance: The variance of the weight image (!), used to account for noisy input
 * - filteredImage: Output image, only pixel (x, y) will be written
 * - rect: The coordinates of the corners of the four images in image space.
 * - r: The half radius of the area over which q is looped
 * - f: The size of the patches that are used for comparing pixels
 * - a: Can be tweaked to account for noisy variance, generally a=1
 * - k_2: Squared k parameter of the NLM filter, general strength control (higher k => smoother image)
 */
ccl_device void kernel_filter_non_local_means(int x, int y, float *noisyImage, float *weightImage, float *variance, float *filteredImage, int4 rect, int r, int f, float a, float k_2)
{
	int2 low  = make_int2(max(rect.x, x - r),
	                      max(rect.y, y - r));
	int2 high = make_int2(min(rect.z, x + r + 1),
	                      min(rect.w, y + r + 1));

	float sum_image = 0.0f, sum_weight = 0.0f;

	int w = align_up(rect.z - rect.x, 4);
	int p_idx = (y-rect.y)*w + (x - rect.x);
	int q_idx = (low.y-rect.y)*w + (low.x-rect.x);
	/* Loop over the q's, center pixels of all relevant patches. */
	for(int qy = low.y; qy < high.y; qy++) {
		for(int qx = low.x; qx < high.x; qx++, q_idx++) {
			int2  low_dPatch = make_int2(max(max(rect.x - qx, rect.x - x),  -f), max(max(rect.y - qy, rect.y - y),  -f));
			int2 high_dPatch = make_int2(min(min(rect.z - qx, rect.z - x), f+1), min(min(rect.w - qy, rect.w - y), f+1));
			int dIdx = low_dPatch.x + low_dPatch.y*w;
			/* Loop over the pixels in the patch.
			 * Note that the patch must be small enough to be fully inside the rect, both at p and q.
			 * Do avoid doing all the coordinate calculations twice, the code here computes both weights at once. */
			float dI = 0.0f;
			for(int dy = low_dPatch.y; dy < high_dPatch.y; dy++) {
				for(int dx = low_dPatch.x; dx < high_dPatch.x; dx++, dIdx++) {
					float diff = weightImage[p_idx+dIdx] - weightImage[q_idx+dIdx];
					dI += (diff*diff - a*(variance[p_idx+dIdx] + min(variance[p_idx+dIdx], variance[q_idx+dIdx]))) * (1.0f / (1e-7f + k_2*(variance[p_idx+dIdx] + variance[q_idx+dIdx])));
				}
				dIdx += w-(high_dPatch.x - low_dPatch.x);
			}
			dI *= 1.0f / ((high_dPatch.x - low_dPatch.x) * (high_dPatch.y - low_dPatch.y));

			float wI = fast_expf(-max(0.0f, dI));
			sum_image += wI*noisyImage[q_idx];
			sum_weight += wI;
		}
		q_idx += w-(high.x-low.x);
	}

	filteredImage[p_idx] = sum_image / sum_weight;
}

CCL_NAMESPACE_END