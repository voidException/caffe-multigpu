#include <stdint.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

#include "caffe/common.hpp"
#include "caffe/data_layers.hpp"
#include "caffe/layer.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"
#include <exception>
// caffe.proto > LayerParameter > WindowDataParameter
//   'source' field specifies the window_file
//   'crop_size' indicates the desired warped size

namespace caffe {

template <typename Dtype>
WindowDataLayer<Dtype>::~WindowDataLayer<Dtype>() {
  this->JoinPrefetchThread();
}

template <typename Dtype>
void WindowDataLayer<Dtype>::DataLayerSetUp(const vector<Blob<Dtype>*>& bottom,
      vector<Blob<Dtype>*>* top) {
  // LayerSetUp runs through the window_file and creates two structures
  // that hold windows: one for foreground (object) windows and one
  // for background (non-object) windows. We use an overlap threshold
  // to decide which is which.

  // window_file format
  // repeated:
  //    # image_index
  //    img_path (abs path)
  //    channels
  //    height
  //    width
  //    num_windows
  //    class_index overlap x1 y1 x2 y2

  LOG(INFO) << "Window data layer:" << std::endl
      << "  foreground (object) overlap threshold: "
      << this->layer_param_.window_data_param().fg_threshold() << std::endl
      << "  background (non-object) overlap threshold: "
      << this->layer_param_.window_data_param().bg_threshold() << std::endl
      << "  foreground sampling fraction: "
      << this->layer_param_.window_data_param().fg_fraction();

  const bool prefetch_needs_rand =
      this->transform_param_.mirror() ||
      this->transform_param_.crop_size();
  if (prefetch_needs_rand) {
    const unsigned int prefetch_rng_seed = caffe_rng_rand();
    prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
  } else {
    prefetch_rng_.reset();
  }

  std::ifstream infile(this->layer_param_.window_data_param().source().c_str());
  CHECK(infile.good()) << "Failed to open window file "
      << this->layer_param_.window_data_param().source() << std::endl;

  map<int, int> label_hist;
  label_hist.insert(std::make_pair(0, 0));

  string hashtag;
  int image_index, channels;
  if (!(infile >> hashtag >> image_index)) {
    LOG(FATAL) << "Window file is empty";
  }
  do {
    CHECK_EQ(hashtag, "#");
    // read image path
    string image_path;
    infile >> image_path;
    // read image dimensions
    vector<int> image_size(3);
    infile >> image_size[0] >> image_size[1] >> image_size[2];
    channels = image_size[0];
    image_database_.push_back(std::make_pair(image_path, image_size));

    // read each box
    int num_windows;
    infile >> num_windows;
    const float fg_threshold =
        this->layer_param_.window_data_param().fg_threshold();
    const float bg_threshold =
        this->layer_param_.window_data_param().bg_threshold();
    for (int i = 0; i < num_windows; ++i) {
      int label, x1, y1, x2, y2,flip;
      float overlap;
      infile >> label >> overlap >> x1 >> y1 >> x2 >> y2 >> flip;

      vector<float> window(WindowDataLayer::NUM);
      window[WindowDataLayer::IMAGE_INDEX] = image_index;
      window[WindowDataLayer::LABEL] = label;
      window[WindowDataLayer::OVERLAP] = overlap;
      
      window[WindowDataLayer::X1] = x1;
      window[WindowDataLayer::Y1] = y1;
      window[WindowDataLayer::X2] = x2;
      window[WindowDataLayer::Y2] = y2;
      window[WindowDataLayer::FLIP] = flip;
      

      // add window to foreground list or background list
      if (overlap >= fg_threshold) {
        int label = window[WindowDataLayer::LABEL];
        CHECK_GT(label, -1); // 0 to be -1, Linjie
        fg_windows_.push_back(window);
        label_hist.insert(std::make_pair(label, 0));
        label_hist[label]++;
      } else if (overlap < bg_threshold) {
        // background window, force label and overlap to 0
        window[WindowDataLayer::LABEL] = 0;
        window[WindowDataLayer::OVERLAP] = 0;
        bg_windows_.push_back(window);
        label_hist[0]++;
      }
    }

    if (image_index % 100 == 0) {
      LOG(INFO) << "num: " << image_index << " "
          << image_path << " "
          << image_size[0] << " "
          << image_size[1] << " "
          << image_size[2] << " "
          << "windows to process: " << num_windows;
    }
  } while (infile >> hashtag >> image_index);

  LOG(INFO) << "Number of images: " << image_index+1;

  for (map<int, int>::iterator it = label_hist.begin();
      it != label_hist.end(); ++it) {
    LOG(INFO) << "class " << it->first << " has " << label_hist[it->first]
              << " samples";
  }

  LOG(INFO) << "Amount of context padding: "
      << this->layer_param_.window_data_param().context_pad();

  LOG(INFO) << "Crop mode: "
      << this->layer_param_.window_data_param().crop_mode();

  // image
  const int crop_size = this->transform_param_.crop_size();
  CHECK_GT(crop_size, 0);
  const int batch_size = this->layer_param_.window_data_param().batch_size();
  (*top)[0]->Reshape(batch_size, channels, crop_size, crop_size);
  this->prefetch_data_.Reshape(batch_size, channels, crop_size, crop_size);

  LOG(INFO) << "output data size: " << (*top)[0]->num() << ","
      << (*top)[0]->channels() << "," << (*top)[0]->height() << ","
      << (*top)[0]->width();
  // datum size
  this->datum_channels_ = (*top)[0]->channels();
  this->datum_height_ = (*top)[0]->height();
  this->datum_width_ = (*top)[0]->width();
  this->datum_size_ =
      (*top)[0]->channels() * (*top)[0]->height() * (*top)[0]->width();
  // label
  (*top)[1]->Reshape(batch_size, 1, 1, 1);
  this->prefetch_label_.Reshape(batch_size, 1, 1, 1);
}

template <typename Dtype>
unsigned int WindowDataLayer<Dtype>::PrefetchRand() {
  CHECK(prefetch_rng_);
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
  return (*prefetch_rng)();
}

// Thread fetching the data
template <typename Dtype>
void WindowDataLayer<Dtype>::InternalThreadEntry() {
  // At each iteration, sample N windows where N*p are foreground (object)
  // windows and N*(1-p) are background (non-object) windows

  Dtype* top_data = this->prefetch_data_.mutable_cpu_data();
  Dtype* top_label = this->prefetch_label_.mutable_cpu_data();
  const Dtype scale = this->layer_param_.window_data_param().scale();
  const int batch_size = this->layer_param_.window_data_param().batch_size();
  const int context_pad = this->layer_param_.window_data_param().context_pad();
  const int crop_size = this->transform_param_.crop_size();
  const bool mirror = this->transform_param_.mirror();
  const float fg_fraction =
      this->layer_param_.window_data_param().fg_fraction();
  const Dtype* mean = this->data_mean_.cpu_data();
  const int mean_off = (this->data_mean_.width() - crop_size) / 2;
  const int mean_width = this->data_mean_.width();
  const int mean_height = this->data_mean_.height();
  cv::Size cv_crop_size(crop_size, crop_size);
  const string& crop_mode = this->layer_param_.window_data_param().crop_mode();

  //bool use_square = (crop_mode == "square") ? true : false;

  // zero out batch
  caffe_set(this->prefetch_data_.count(), Dtype(0), top_data);

 

  int item_id = 0;
  // sample from bg set then fg set
  int is_fg = 1;
  for (int dummy = 0; dummy < batch_size; ++dummy) {
    // sample a window
    const unsigned int rand_index = PrefetchRand();
    vector<float> window = fg_windows_[rand_index % fg_windows_.size()];
    

    // load the image containing the window
    // LOG(INFO) << "Read window info ";
    pair<std::string, vector<int> > image;
    if (window[WindowDataLayer<Dtype>::IMAGE_INDEX] >= image_database_.size()) {
      LOG(ERROR) << "index exceed image_database_'s size ";
      LOG(ERROR) << " image index: "<<window[WindowDataLayer<Dtype>::IMAGE_INDEX] 
      <<"database size: "<<image_database_.size();
    }
    
    image =
    image_database_[window[WindowDataLayer<Dtype>::IMAGE_INDEX]];
   
    cv::Mat cv_img = cv::imread(image.first, CV_LOAD_IMAGE_COLOR);
    if (!cv_img.data) {
      LOG(ERROR) << "Could not open or find file " << image.first;
      return;
    }
    const int channels = cv_img.channels();

    // crop window out of image and warp it
    int x1 = window[WindowDataLayer<Dtype>::X1];
    int y1 = window[WindowDataLayer<Dtype>::Y1];
    int x2 = window[WindowDataLayer<Dtype>::X2];
    int y2 = window[WindowDataLayer<Dtype>::Y2];

    int pad_w = 0;
    int pad_h = 0;
    

    cv::Rect roi(x1, y1, x2-x1+1, y2-y1+1);
    //LOG(INFO) << "cv crop ";
    cv::Mat cv_cropped_img = cv_img(roi);
    // useful code to debug
    // try {
    //   cv_cropped_img = cv_img(roi);
    // } catch (std::exception &e) {
    //   LOG(ERROR) << "cv_img crop error, roi is " << x1 << " " << y1 << " " << x2 << " " << y2 <<std::endl;
    //   LOG(ERROR) << "original roi is " << window[WindowDataLayer<Dtype>::X1] << " " 
    //   << window[WindowDataLayer<Dtype>::Y1] << " " << window[WindowDataLayer<Dtype>::X2] << " " 
    //   << window[WindowDataLayer<Dtype>::Y2] <<std::endl;
    //   LOG(ERROR) << "imsize is " << image.second[1] <<" "<< image.second[2] <<std::endl;
    //   LOG(ERROR) << " im path is " << image.first <<std::endl;
    //   LOG(ERROR) << e.what() << std::endl;
    // } 
    cv::resize(cv_cropped_img, cv_cropped_img,
        cv_crop_size, 0, 0, cv::INTER_LINEAR);

    // horizontal flip at random
    if (window[WindowDataLayer<Dtype>::FLIP]) { //original: do_mirror Linjie
      cv::flip(cv_cropped_img, cv_cropped_img, 1);
    }
    // try {
    // copy the warped window into top_data
    //LOG(INFO) << "Start copy to top_data ";
    for (int c = 0; c < channels; ++c) {
      for (int h = 0; h < cv_cropped_img.rows; ++h) {
        for (int w = 0; w < cv_cropped_img.cols; ++w) {
          Dtype pixel =
              static_cast<Dtype>(cv_cropped_img.at<cv::Vec3b>(h, w)[c]);

          top_data[((item_id * channels + c) * crop_size + h + pad_h)
                   * crop_size + w + pad_w]
              = (pixel
                  - mean[(c * mean_height + h + mean_off + pad_h)
                         * mean_width + w + mean_off + pad_w])
                * scale;
        }
      }
    }
    // } catch (std::exception &e) {
    //   LOG(ERROR)<< "copy image into top_data error" <<std::endl;
    // }
    // get window label
    top_label[item_id] = window[WindowDataLayer<Dtype>::LABEL];
    //LOG(INFO) << "Reach end of function ";
    #if 0
    // useful debugging code for dumping transformed windows to disk
    string file_id;
    std::stringstream ss;
    ss << PrefetchRand();
    ss >> file_id;
    std::ofstream inf((string("dump/") + file_id +
        string("_info.txt")).c_str(), std::ofstream::out);
    inf << image.first << std::endl
        << window[WindowDataLayer<Dtype>::X1]+1 << std::endl
        << window[WindowDataLayer<Dtype>::Y1]+1 << std::endl
        << window[WindowDataLayer<Dtype>::X2]+1 << std::endl
        << window[WindowDataLayer<Dtype>::Y2]+1 << std::endl
        << window[WindowDataLayer<Dtype>::FLIP] << std::endl
        << top_label[item_id] << std::endl
        << is_fg << std::endl;
    inf.close();
    std::ofstream top_data_file((string("dump/") + file_id +
        string("_data.txt")).c_str(),
        std::ofstream::out | std::ofstream::binary);
    for (int c = 0; c < channels; ++c) {
      for (int h = 0; h < crop_size; ++h) {
        for (int w = 0; w < crop_size; ++w) {
          top_data_file.write(reinterpret_cast<char*>(
              &top_data[((item_id * channels + c) * crop_size + h)
                        * crop_size + w]),
              sizeof(Dtype));
        }
      }
    }
    top_data_file.close();
    #endif

    item_id++;
  }
  
}

INSTANTIATE_CLASS(WindowDataLayer);

}  // namespace caffe
