#include "IdentifyOperTask.h"

#include <functional>
#include <thread>
#include <future>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <opencv2/opencv.hpp>

#include "Configer.h"
#include "InfrastConfiger.h"
#include "Identify.h"
#include "WinMacro.h"

using namespace asst;

asst::IdentifyOperTask::IdentifyOperTask(AsstCallback callback, void* callback_arg)
	: OcrAbstractTask(callback, callback_arg)
{
	;
}

bool asst::IdentifyOperTask::run()
{
	if (m_view_ptr == nullptr
		|| m_identify_ptr == nullptr
		|| m_control_ptr == nullptr)
	{
		m_callback(AsstMsg::PtrIsNull, json::value(), m_callback_arg);
		return false;
	}

	json::value task_start_json = json::object{
		{ "task_type",  "InfrastStationTask" },
		{ "task_chain", OcrAbstractTask::m_task_chain},
	};
	m_callback(AsstMsg::TaskStart, task_start_json, m_callback_arg);

	std::unordered_map<std::string, std::string> feature_cond = InfrastConfiger::get_instance().m_oper_name_feat;
	std::unordered_set<std::string> feature_whatever = InfrastConfiger::get_instance().m_oper_name_feat_whatever;
	std::unordered_set<OperInfrastInfo> detected_opers;

	// 一边识别一边滑动，把所有干员名字抓出来
	// 异步进行滑动操作
	m_keep_swipe = true;
	std::future<bool> swipe_future = std::async(
		std::launch::async, &IdentifyOperTask::keep_swipe, this, false);
	while (true) {
		const cv::Mat& image = OcrAbstractTask::get_format_image(true);

		auto cur_name_textarea = detect_opers(image, feature_cond, feature_whatever);

		int oper_numer = detected_opers.size();
		for (const TextArea& textarea : cur_name_textarea)
		{
			int elite = detect_elite(image, textarea.rect);
			if (elite == -1) {
				continue;
			}
			OperInfrastInfo info;
			info.elite = elite;
			info.name = textarea.text;
			detected_opers.emplace(std::move(info));
		}

		json::value opers_json;
		std::vector<json::value> opers_json_vec;
		for (const OperInfrastInfo& info : detected_opers) {
			json::value info_json;
			info_json["name"] = Utf8ToGbk(info.name);
			info_json["elite"] = info.elite;
			//info_json["level"] = info.level;
			opers_json_vec.emplace_back(std::move(info_json));
		}
		opers_json["all"] = json::array(opers_json_vec);
		m_callback(AsstMsg::InfrastOpers, opers_json, m_callback_arg);

		// 说明本次识别一个新的都没识别到，应该是滑动到最后了，直接结束循环
		if (oper_numer == detected_opers.size()) {
			break;
		}
		if (need_exit()) {
			return false;
		}
	}
	// 等待滑动结束
	m_keep_swipe = false;
	swipe_future.wait();

	return true;
}

std::vector<TextArea> asst::IdentifyOperTask::detect_opers(const cv::Mat& image, std::unordered_map<std::string, std::string>& feature_cond, std::unordered_set<std::string>& feature_whatever)
{
	// 裁剪出来干员名的一个长条形图片，没必要把整张图片送去识别
	// TODO，这个参数要根据分辨率调整
	constexpr static int cropped_height = 100;
	constexpr static int cropped_upper_y = 665;
	cv::Mat upper_part_name_image = image(cv::Rect(0, cropped_upper_y, image.cols, cropped_height));

	std::vector<TextArea> upper_text_area = ocr_detect(upper_part_name_image);	// 所有文字
	// 因为图片是裁剪过的，所以对应原图的坐标要加上裁剪的参数
	for (TextArea& textarea : upper_text_area) {
		textarea.rect.y += cropped_upper_y;
	}
	// 过滤出所有的干员名
	std::vector<TextArea> upper_part_names = text_match(
		upper_text_area,
		InfrastConfiger::get_instance().m_all_opers_name,
		Configer::get_instance().m_infrast_ocr_replace);

	// 下半部分的干员
	// TODO，这个y参数要根据分辨率调整
	constexpr static int cropped_lower_y = 1300;
	cv::Mat lower_part_name_image = image(cv::Rect(0, cropped_lower_y, image.cols, cropped_height));
	std::vector<TextArea> lower_text_area = ocr_detect(lower_part_name_image);	// 所有文字
	// 因为图片是裁剪过的，所以对应原图的坐标要加上裁剪的参数
	for (TextArea& textarea : lower_text_area) {
		textarea.rect.y += cropped_lower_y;
	}
	// 过滤出所有的干员名
	std::vector<TextArea> lower_part_names = text_match(
		lower_text_area,
		InfrastConfiger::get_instance().m_all_opers_name,
		Configer::get_instance().m_infrast_ocr_replace);

	// 上下两部分识别结果合并
	std::vector<TextArea> all_text_area = std::move(upper_text_area);
	all_text_area.insert(all_text_area.end(),
		std::make_move_iterator(lower_text_area.begin()),
		std::make_move_iterator(lower_text_area.end()));
	std::vector<TextArea> all_opers_textarea = std::move(upper_part_names);
	all_opers_textarea.insert(all_opers_textarea.end(),
		std::make_move_iterator(lower_part_names.begin()),
		std::make_move_iterator(lower_part_names.end()));

	// 如果ocr结果中已经有某个干员了，就没必要再尝试对他特征检测了，直接删了
	for (const TextArea& textarea : all_opers_textarea) {
		auto cond_iter = std::find_if(feature_cond.begin(), feature_cond.end(),
			[&textarea](const auto& pair) -> bool {
				return textarea.text == pair.second;
			});
		if (cond_iter != feature_cond.end()) {
			feature_cond.erase(cond_iter);
		}

		auto whatever_iter = std::find_if(feature_whatever.begin(), feature_whatever.end(),
			[&textarea](const std::string& str) -> bool {
				return textarea.text == str;
			});
		if (whatever_iter != feature_whatever.end()) {
			feature_whatever.erase(whatever_iter);
		}
	}

	// 用特征检测再筛选一遍OCR识别漏了的——有关键字的
	for (const TextArea& textarea : all_text_area) {
		for (auto iter = feature_cond.begin(); iter != feature_cond.end(); ++iter) {
			auto& [key, value] = *iter;
			// 识别到了key，但是没识别到value，这种情况就需要进行特征检测进一步确认了
			if (textarea.text.find(key) != std::string::npos
				&& textarea.text.find(value) == std::string::npos) {
				// 把key所在的矩形放大一点送去做特征检测，不需要把整张图片都送去检测
				Rect magnified_area = textarea.rect.center_zoom(2.0);
				magnified_area.x = (std::max)(0, magnified_area.x);
				magnified_area.y = (std::max)(0, magnified_area.y);
				if (magnified_area.x + magnified_area.width >= image.cols) {
					magnified_area.width = image.cols - magnified_area.x - 1;
				}
				if (magnified_area.y + magnified_area.height >= image.rows) {
					magnified_area.height = image.rows - magnified_area.y - 1;
				}
				cv::Rect cv_rect(magnified_area.x, magnified_area.y, magnified_area.width, magnified_area.height);
				// key是关键字而已，真正要识别的是value
				auto&& ret = OcrAbstractTask::m_identify_ptr->feature_match(image(cv_rect), value);
				if (ret) {
					// 匹配上了下次就不用再匹配这个了，直接删了
					all_opers_textarea.emplace_back(value, textarea.rect);
					iter = feature_cond.erase(iter);
					--iter;
					// 也从whatever里面删了
					auto whatever_iter = std::find_if(feature_whatever.begin(), feature_whatever.end(),
						[&textarea](const std::string& str) -> bool {
							return textarea.text == str;
						});
					if (whatever_iter != feature_whatever.end()) {
						feature_whatever.erase(whatever_iter);
					}
				}
			}
		}
	}

	// 用特征检测再筛选一遍OCR识别漏了的——无论如何都进行识别的
	for (auto iter = feature_whatever.begin(); iter != feature_whatever.end(); ++iter) {
		// 上半部分长条形的图片
		auto&& upper_ret = OcrAbstractTask::m_identify_ptr->feature_match(upper_part_name_image, *iter);
		if (upper_ret) {
			// 因为图片是裁剪过的，所以对应原图的坐标要加上裁剪的参数
			TextArea temp = std::move(upper_ret.value());
			temp.rect.y += cropped_upper_y;
			all_opers_textarea.emplace_back(std::move(temp));
			iter = feature_whatever.erase(iter);
			--iter;
			continue;
		}
		// 下半部分长条形的图片
		auto&& lower_ret = OcrAbstractTask::m_identify_ptr->feature_match(lower_part_name_image, *iter);
		if (lower_ret) {
			// 因为图片是裁剪过的，所以对应原图的坐标要加上裁剪的参数
			TextArea temp = std::move(lower_ret.value());
			temp.rect.y += cropped_lower_y;
			all_opers_textarea.emplace_back(std::move(temp));
			iter = feature_whatever.erase(iter);
			--iter;
			continue;
		}
	}

	return all_opers_textarea;
}

int asst::IdentifyOperTask::detect_elite(const cv::Mat& image, const asst::Rect name_rect)
{
	cv::Rect elite_rect;
	// 因为有的名字长有的名字短，但是右对齐的，所以跟着右边走
	// TODO，这些长宽的参数要跟着分辨率缩放，最好放到配置文件里
	elite_rect.x = name_rect.x + name_rect.width - 250;
	elite_rect.y = name_rect.y - 200;
	if (elite_rect.x < 0 || elite_rect.y < 0) {
		return -1;
	}
	elite_rect.width = 100;
	elite_rect.height = 150;
	cv::Mat elite_mat = image(elite_rect);

	// for debug
	static cv::Mat elite1 = cv::imread(GetResourceDir() + "operators\\Elite1.png");
	static cv::Mat elite2 = cv::imread(GetResourceDir() + "operators\\Elite2.png");
	auto&& [score1, point1] = OcrAbstractTask::m_identify_ptr->match_template(elite_mat, elite1);
	auto&& [score2, point2] = OcrAbstractTask::m_identify_ptr->match_template(elite_mat, elite2);
	if (score1 > score2 && score1 > 0.7) {
		return 1;
	}
	else if (score2 > score1 && score2 > 0.7) {
		return 2;
	}
	else {
		return 0;
	}
}

bool IdentifyOperTask::keep_swipe(bool reverse)
{
	bool ret = false;
	while (m_keep_swipe && !ret) {
		if (!reverse) {
			ret &= m_control_ptr->swipe(m_swipe_begin, m_swipe_end, m_swipe_duration);
		}
		else {
			ret &= m_control_ptr->swipe(m_swipe_end, m_swipe_begin, m_swipe_duration);
		}
		ret &= sleep(SwipeExtraDelay);
	}
	return ret;
}