#define W2XCONV_IMPL

#include "w2xconv.h"
#include "Buffer.hpp"
#include "ModelHandler.hpp"
#include "convertRoutine.hpp"

struct W2XConvImpl {
	std::string dev_name;

	ComputeEnv env;

	std::vector<std::unique_ptr<w2xc::Model> > noise1_models;
	std::vector<std::unique_ptr<w2xc::Model> > noise2_models;
	std::vector<std::unique_ptr<w2xc::Model> > scale2_models;
};

W2XConv *
w2xconv_init(int enable_gpu,
             int nJob,
	     int enable_log)
{
	struct W2XConv *c = new struct W2XConv;
	struct W2XConvImpl *impl = new W2XConvImpl;

	c->impl = impl;
	c->enable_log = enable_log;

	if (nJob == 0) {
		nJob = std::thread::hardware_concurrency();
	}

	impl->env.tpool = w2xc::initThreadPool(nJob);

	if (enable_gpu) {
		w2xc::initOpenCL(&impl->env);
		w2xc::initCUDA(&impl->env);
	}

	c->last_error.code = W2XCONV_NOERROR;
	c->flops.flop = 0;
	c->flops.filter_sec = 0;
	c->flops.process_sec = 0;

	if (impl->env.num_cuda_dev != 0) {
		c->target_processor.type = W2XCONV_PROC_CUDA;
		impl->dev_name = impl->env.cuda_dev_list[0].name.c_str();
	} else if (impl->env.num_cl_dev != 0) {
		c->target_processor.type = W2XCONV_PROC_OPENCL;
		impl->dev_name = impl->env.cl_dev_list[0].name.c_str();
	} else {
		{
			int v[4];
			char name[1024];

			__cpuid(v, 0x80000000);
			if ((unsigned int)v[0] >= 0x80000004) {
				__cpuid((int*)(name+0), 0x80000002);
				__cpuid((int*)(name+16), 0x80000003);
				__cpuid((int*)(name+32), 0x80000004);
				name[48] = '\0';
			} else {
				char data[16];
				int data4[4];
				__cpuid((int*)data, 0x0);
				__cpuid(data4, 0x1);

				name[0] = data[4];
				name[1] = data[5];
				name[2] = data[6];
				name[3] = data[7];

				name[4] = data[12];
				name[5] = data[13];
				name[6] = data[14];
				name[7] = data[15];

				name[8] = data[8];
				name[9] = data[9];
				name[10] = data[10];
				name[11] = data[11];

				name[12] = '\0';
			}

			c->target_processor.type = W2XCONV_PROC_HOST;
			impl->dev_name = name;
		}
	}

	c->target_processor.dev_name = impl->dev_name.c_str();

	return c;
}

static void
clearError(W2XConv *conv)
{
	switch (conv->last_error.code) {
	case W2XCONV_NOERROR:
	case W2XCONV_ERROR_WIN32_ERROR:
	case W2XCONV_ERROR_LIBC_ERROR:
		break;

	case W2XCONV_ERROR_WIN32_ERROR_PATH:
		free(conv->last_error.u.win32_path.path);
		break;

	case W2XCONV_ERROR_LIBC_ERROR_PATH:
		free(conv->last_error.u.libc_path.path);
		break;

	case W2XCONV_ERROR_MODEL_LOAD_FAILED:
	case W2XCONV_ERROR_IMREAD_FAILED:
	case W2XCONV_ERROR_IMWRITE_FAILED:
		free(conv->last_error.u.path);
		break;
	}
}

char *
w2xconv_strerror(W2XConvError *e)
{
	std::ostringstream oss;
	char *str;

	switch (e->code) {
	case W2XCONV_NOERROR:
		oss << "no error";
		break;

	case W2XCONV_ERROR_WIN32_ERROR:
		oss << "win32_err: " << e->u.errno_;
		break;

	case W2XCONV_ERROR_WIN32_ERROR_PATH:
		oss << "win32_err: " << e->u.win32_path.errno_ << "(" << e->u.win32_path.path << ")";
		break;

	case W2XCONV_ERROR_LIBC_ERROR:
		oss << strerror(e->u.errno_);
		break;

	case W2XCONV_ERROR_LIBC_ERROR_PATH:
		str = strerror(e->u.libc_path.errno_);
		oss << str << "(" << e->u.libc_path.path << ")";
		break;

	case W2XCONV_ERROR_MODEL_LOAD_FAILED:
		oss << "model load failed: " << e->u.path;
		break;

	case W2XCONV_ERROR_IMREAD_FAILED:
		oss << "cv::imread(\"" << e->u.path << "\") failed";
		break;

	case W2XCONV_ERROR_IMWRITE_FAILED:
		oss << "cv::imwrite(\"" << e->u.path << "\") failed";
		break;
	}

	return strdup(oss.str().c_str());
}

void
w2xconv_free(void *p)
{
	free(p);
}


static void
setPathError(W2XConv *conv,
             enum W2XConvErrorCode code,
             std::string const &path)
{
	clearError(conv);

	conv->last_error.code = code;
	conv->last_error.u.path = strdup(path.c_str());
}

int
w2xconv_load_models(W2XConv *conv, const char *model_dir)
{
	struct W2XConvImpl *impl = conv->impl;

	std::string modelFileName(model_dir);

	impl->noise1_models.clear();
	impl->noise2_models.clear();
	impl->scale2_models.clear();

	if (!w2xc::modelUtility::generateModelFromJSON(modelFileName + "/noise1_model.json", impl->noise1_models)) {
		setPathError(conv,
			     W2XCONV_ERROR_MODEL_LOAD_FAILED,
			     modelFileName + "/noise1_model.json");
		return -1;
	}
	if (!w2xc::modelUtility::generateModelFromJSON(modelFileName + "/noise2_model.json", impl->noise2_models)) {
		setPathError(conv,
			     W2XCONV_ERROR_MODEL_LOAD_FAILED,
			     modelFileName + "/noise2_model.json");
		return -1;
	}
	if (!w2xc::modelUtility::generateModelFromJSON(modelFileName + "/scale2.0x_model.json", impl->scale2_models)) {
		setPathError(conv,
			     W2XCONV_ERROR_MODEL_LOAD_FAILED,
			     modelFileName + "/scale2.0x_model.json");
		return -1;

	}

	return 0;
}


void
w2xconv_fini(struct W2XConv *conv)
{
	struct W2XConvImpl *impl = conv->impl;
	clearError(conv);

	w2xc::finiCUDA(&impl->env);
	w2xc::finiOpenCL(&impl->env);
	w2xc::finiThreadPool(impl->env.tpool);

	delete impl;
	delete conv;
}

static void
apply_denoise(struct W2XConv *conv,
	      cv::Mat &image,
	      int denoise_level,
	      cv::Size &bs)
{
	struct W2XConvImpl *impl = conv->impl;
	ComputeEnv *env = &impl->env;

	std::vector<cv::Mat> imageSplit;
	cv::Mat imageY;
	cv::split(image, imageSplit);
	imageSplit[0].copyTo(imageY);

	if (denoise_level == 1) {
		w2xc::convertWithModels(env, imageY, imageSplit[0],
					impl->noise1_models,
					&conv->flops, bs, conv->enable_log);
	} else {
		w2xc::convertWithModels(env, imageY, imageSplit[0],
					impl->noise2_models,
					&conv->flops, bs, conv->enable_log);
	}

	cv::merge(imageSplit, image);
}

static void
apply_scale(struct W2XConv *conv,
	    cv::Mat &image,
	    int iterTimesTwiceScaling,
	    cv::Size &bs)
{
	struct W2XConvImpl *impl = conv->impl;
	ComputeEnv *env = &impl->env;

	if (conv->enable_log) {
		std::cout << "start scaling" << std::endl;
	}

	// 2x scaling
	for (int nIteration = 0; nIteration < iterTimesTwiceScaling;
	     nIteration++) {

		if (conv->enable_log) {
			std::cout << "#" << std::to_string(nIteration + 1)
				  << " 2x scaling..." << std::endl;
		}

		cv::Size imageSize = image.size();
		imageSize.width *= 2;
		imageSize.height *= 2;
		cv::Mat image2xNearest;
		cv::resize(image, image2xNearest, imageSize, 0, 0, cv::INTER_NEAREST);
		std::vector<cv::Mat> imageSplit;
		cv::Mat imageY;
		cv::split(image2xNearest, imageSplit);
		imageSplit[0].copyTo(imageY);
		// generate bicubic scaled image and
		// convert RGB -> YUV and split
		imageSplit.clear();
		cv::Mat image2xBicubic;
		cv::resize(image,image2xBicubic,imageSize,0,0,cv::INTER_CUBIC);
		cv::split(image2xBicubic, imageSplit);

		if(!w2xc::convertWithModels(env, imageY, imageSplit[0],
					    impl->scale2_models,
					    &conv->flops, bs, conv->enable_log)){
			std::cerr << "w2xc::convertWithModels : something error has occured.\n"
				"stop." << std::endl;
			std::exit(1);
		};

		cv::merge(imageSplit, image);

	} // 2x scaling : end
}

int
w2xconv_convert_file(struct W2XConv *conv,
		     const char *dst_path,
                     const char *src_path,
                     int denoise_level,
                     double scale,
		     int block_size)
{
	struct W2XConvImpl *impl = conv->impl;
	ComputeEnv *env = &impl->env;

	cv::Mat image = cv::imread(src_path, cv::IMREAD_COLOR);
	if (image.data == nullptr) {
		setPathError(conv,
			     W2XCONV_ERROR_IMREAD_FAILED,
			     src_path);
		return -1;
	}

	image.convertTo(image, CV_32F, 1.0 / 255.0);
	cv::cvtColor(image, image, cv::COLOR_RGB2YUV);
	cv::Size bs(block_size, block_size);

	if (denoise_level != 0) {
		apply_denoise(conv, image, denoise_level, bs);
	}

	if (scale != 1.0) {
		// calculate iteration times of 2x scaling and shrink ratio which will use at last
		int iterTimesTwiceScaling = static_cast<int>(std::ceil(std::log2(scale)));
		double shrinkRatio = 0.0;
		if (static_cast<int>(scale)
		    != std::pow(2, iterTimesTwiceScaling))
		{
			shrinkRatio = scale / std::pow(2.0, static_cast<double>(iterTimesTwiceScaling));
		}

		apply_scale(conv, image, iterTimesTwiceScaling, bs);

		if (shrinkRatio != 0.0) {
			cv::Size lastImageSize = image.size();
			lastImageSize.width =
				static_cast<int>(static_cast<double>(lastImageSize.width
								     * shrinkRatio));
			lastImageSize.height =
				static_cast<int>(static_cast<double>(lastImageSize.height
								     * shrinkRatio));
			cv::resize(image, image, lastImageSize, 0, 0, cv::INTER_LINEAR);
		}
	}

	cv::cvtColor(image, image, cv::COLOR_YUV2RGB);
	image.convertTo(image, CV_8U, 255.0);

	if (!cv::imwrite(dst_path, image)) {
		setPathError(conv,
			     W2XCONV_ERROR_IMWRITE_FAILED,
			     dst_path);
		return -1;
	}

	return 0;
}

static void
convert_yuv(struct W2XConv *conv,
	    cv::Mat &image,
	    int denoise_level,
	    double scale,
	    int dst_w, int dst_h,
	    int block_size)
{
	cv::Size bs(block_size, block_size);

	if (denoise_level != 0) {
		apply_denoise(conv, image, denoise_level, bs);
	}

	if (scale != 1.0) {
		// calculate iteration times of 2x scaling and shrink ratio which will use at last
		int iterTimesTwiceScaling = static_cast<int>(std::ceil(std::log2(scale)));
		double shrinkRatio = 0.0;
		if (static_cast<int>(scale)
		    != std::pow(2, iterTimesTwiceScaling))
		{
			shrinkRatio = scale / std::pow(2.0, static_cast<double>(iterTimesTwiceScaling));
		}

		apply_scale(conv, image, iterTimesTwiceScaling, bs);

		if (shrinkRatio != 0.0) {
			cv::Size lastImageSize = image.size();
			lastImageSize.width = dst_w;
			lastImageSize.height = dst_h;
			cv::resize(image, image, lastImageSize, 0, 0, cv::INTER_LINEAR);
		}
	}
}

int
w2xconv_convert_rgb(struct W2XConv *conv,
		    unsigned char *dst, size_t dst_step_byte, /* rgb24 (src_w*ratio, src_h*ratio) */
		    unsigned char *src, size_t src_step_byte, /* rgb24 (src_w, src_h) */
		    int src_w, int src_h,
		    int denoise_level, /* 0:none, 1:L1 denoise, other:L2 denoise  */
		    double scale,
		    int block_size)
{
	int dst_h = src_h * scale;
	int dst_w = src_w * scale;

	cv::Mat dsti(src_w, src_h, CV_8UC3, src_step_byte);
	cv::Mat srci(dst_w, dst_h, CV_8UC3, dst_step_byte);

	cv::Mat image;

	srci.convertTo(image, CV_32F, 1.0 / 255.0);
	cv::cvtColor(image, image, cv::COLOR_RGB2YUV);

	convert_yuv(conv, image, denoise_level, scale, dst_h, dst_h, block_size);

	cv::cvtColor(image, image, cv::COLOR_YUV2RGB);
	image.convertTo(dsti, CV_8U, 255.0);

	return 0;
}

int
w2xconv_convert_yuv(struct W2XConv *conv,
		    unsigned char *dst, size_t dst_step_byte, /* float32x3 (src_w*ratio, src_h*ratio) */
		    unsigned char *src, size_t src_step_byte, /* float32x3 (src_w, src_h) */
		    int src_w, int src_h,
		    int denoise_level, /* 0:none, 1:L1 denoise, other:L2 denoise  */
		    double scale,
		    int block_size)
{
	int dst_h = src_h * scale;
	int dst_w = src_w * scale;

	cv::Mat dsti(src_w, src_h, CV_32FC3, src_step_byte);
	cv::Mat srci(dst_w, dst_h, CV_32FC3, dst_step_byte);

	cv::Mat image = srci.clone();

	convert_yuv(conv, image, denoise_level, scale, dst_h, dst_h, block_size);

	image.copyTo(dsti);

	return 0;
}


int
w2xconv_apply_filter_y(struct W2XConv *conv,
		       enum W2XConvFilterType type,
		       unsigned char *dst, size_t dst_step_byte, /* float32x1 (src_w, src_h) */
		       unsigned char *src, size_t src_step_byte, /* float32x1 (src_w, src_h) */
		       int src_w, int src_h,
		       int block_size)
{
	struct W2XConvImpl *impl = new W2XConvImpl;
	ComputeEnv *env = &impl->env;

	cv::Mat dsti(src_w, src_h, CV_32F, dst_step_byte);
	cv::Mat srci(src_w, src_h, CV_32F, src_step_byte);

	cv::Size bs(block_size, block_size);

	std::vector<std::unique_ptr<w2xc::Model> > *mp;

	switch (type) {
	case W2XCONV_FILTER_DENOISE1:
		mp = &impl->noise1_models;
		break;

	case W2XCONV_FILTER_DENOISE2:
		mp = &impl->noise2_models;
		break;

	case W2XCONV_FILTER_SCALE2x:
		mp = &impl->scale2_models;
		break;
	}

	w2xc::convertWithModels(env, dsti, srci,
				*mp,
				&conv->flops, bs, conv->enable_log);

	return 0;
}
