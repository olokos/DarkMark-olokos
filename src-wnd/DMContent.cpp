/* DarkMark (C) 2019 Stephane Charette <stephanecharette@gmail.com>
 * $Id$
 */

#include "DarkMark.hpp"

#include "json.hpp"
using json = nlohmann::json;


dm::DMContent::DMContent() :
	canvas(*this),
	sort_order(static_cast<ESort>(cfg().get_int("sort_order"))),
	show_labels(static_cast<EToggle>(cfg().get_int("show_labels"))),
	alpha_blend_percentage(static_cast<double>(cfg().get_int("alpha_blend_percentage")) / 100.0),
	show_predictions(false),
	need_to_save(false),
	selected_mark(-1),
	most_recent_class_idx(0),
	scale_factor(1.0),
	image_directory(cfg().get_str("image_directory")),
	image_filename_index(0)
{
	corners.push_back(new DMCorner(*this, ECorner::kTL));
	corners.push_back(new DMCorner(*this, ECorner::kTR));
	corners.push_back(new DMCorner(*this, ECorner::kBR));
	corners.push_back(new DMCorner(*this, ECorner::kBL));

	addAndMakeVisible(canvas);

	for (auto c : corners)
	{
		addAndMakeVisible(c);
	}

	setWantsKeyboardFocus(true);

	const std::regex image_filename_regex(cfg().get_str("image_regex"), std::regex::icase | std::regex::nosubs | std::regex::optimize | std::regex::ECMAScript);

	DirectoryIterator iter(image_directory, true, "*", File::findFiles + File::ignoreHiddenFiles);
	while (iter.next())
	{
		File f = iter.getFile();
		const std::string filename = f.getFullPathName().toStdString();
		if (std::regex_match(filename, image_filename_regex))
		{
			if (filename.find("chart.png") == std::string::npos)
			{
				image_filenames.push_back(filename);
			}
		}
	}
	Log("number of images found in " + image_directory.getFullPathName().toStdString() + ": " + std::to_string(image_filenames.size()));
	set_sort_order(sort_order);

	return;
}


dm::DMContent::~DMContent(void)
{
	if (need_to_save)
	{
		save_json();
		save_text();
	}

	for (auto c : corners)
	{
		delete c;
	}
	corners.clear();

	return;
}


void dm::DMContent::resized()
{
	const double window_width	= getWidth();
	const double window_height	= getHeight();
	if(	window_width	< 1.0 ||
		window_height	< 1.0 )
	{
		// window hasn't been created yet?
		return;
	}

	double image_width	= original_image.cols;
	double image_height	= original_image.rows;
	if (image_width		< 1.0 ||
		image_height	< 1.0 )
	{
		// image hasn't been loaded yet?
		image_width = 640;
		image_height = 480;
	}

	// the wider the window, the larger the cells should be
	const double pixels_per_cell				= std::floor(std::max(6.0, window_width / 85.0));	// 600=7, 800=9, 1000=11, 1600=18, ...

	const double min_number_of_cells			= 5.0;
	const double min_corner_width				= min_number_of_cells * (pixels_per_cell + 1);
	const double number_of_corner_windows		= corners.size();
	const double min_horizontal_spacer_height	= 2.0;

	// determine the size of the image once it is scaled
	const double width_ratio					= (window_width - min_corner_width) / image_width;
	const double height_ratio					= window_height / image_height;
	const double ratio							= std::min(width_ratio, height_ratio);
	const double new_image_width				= std::round(ratio * image_width);
	const double new_image_height				= std::round(ratio * image_height);

	// determine the size of each corner window
	const double max_corner_width				= std::floor(window_width - new_image_width);
	const double max_corner_height				= std::floor(window_height / number_of_corner_windows - (min_horizontal_spacer_height * (number_of_corner_windows - 1.0)));
	const double number_of_horizontal_cells		= std::floor(max_corner_width	/ (pixels_per_cell + 1));
	const double number_of_vertical_cells		= std::floor(max_corner_height	/ (pixels_per_cell + 1));
	const double new_corner_width				= number_of_horizontal_cells	* (pixels_per_cell + 1) - 1;
	const double new_corner_height				= number_of_vertical_cells		* (pixels_per_cell + 1) - 1;
	const double horizontal_spacer_height		= std::floor((window_height - new_corner_height * number_of_corner_windows) / (number_of_corner_windows - 1.0));

#if 0	// enable this to debug resizing
	static size_t counter = 0;
	counter ++;
	if (counter % 10 == 0)
	{
		asm("int $3");
	}

	Log("resized:"
			" window size: "		+ std::to_string((int)window_width)		+ "x" + std::to_string((int)window_height)		+
			" new image size: "		+ std::to_string((int)new_image_width)	+ "x" + std::to_string((int)new_image_height)	+
			" min corner size: "	+ std::to_string((int)min_corner_width)	+ "x" + std::to_string((int)new_corner_height)	+
			" max corner size: "	+ std::to_string((int)max_corner_width)	+ "x" + std::to_string((int)max_corner_height)	+
			" new corner size: "	+ std::to_string((int)new_corner_width)	+ "x" + std::to_string((int)new_corner_height)	+
			" horizontal spacer: "	+ std::to_string((int)horizontal_spacer_height)											+
			" pixels per cell: "	+ std::to_string((int)pixels_per_cell)													+
			" horizontal cells: "	+ std::to_string((int)number_of_horizontal_cells)										+
			" vertical cells: "		+ std::to_string((int)number_of_vertical_cells)											);
#endif

	canvas.setBounds(0, 0, new_image_width, new_image_height);
	int y = 0;
	for (auto c : corners)
	{
		c->setBounds(new_image_width, y, new_corner_width, new_corner_height);
		y += new_corner_height + horizontal_spacer_height;
		c->cell_size = pixels_per_cell;
		c->cols = number_of_horizontal_cells;
		c->rows = number_of_vertical_cells;
	}

	// remember some of the important numbers so we don't have to re-calculate them later
	scaled_image_size = cv::Size(new_image_width, new_image_height);
	scale_factor = ratio;

	// update the window title to show the scale factor
	if (dmapp().wnd)
	{
		// since we're going to be messing around with the window title, make a copy of the original window name

		static const std::string original_title = dmapp().wnd->getName().toStdString();

		std::string title =
			original_title +
			" - "	+ std::to_string(1 + image_filename_index) + "/" + std::to_string(image_filenames.size()) +
			" - "	+ short_filename +
			" - "	+ std::to_string(original_image.cols) +
			"x"		+ std::to_string(original_image.rows) +
			" - "	+ std::to_string(static_cast<int>(std::round(scale_factor * 100.0))) + "%";

		dmapp().wnd->setName(title);
	}

	return;
}


void dm::DMContent::start_darknet()
{
	Log("loading darknet neural network");
	const std::string darknet_cfg		= cfg().get_str("darknet_config"	);
	const std::string darknet_weights	= cfg().get_str("darknet_weights"	);
	const std::string darknet_names		= cfg().get_str("darknet_names"		);
	try
	{
		dmapp().darkhelp.reset(new DarkHelp(darknet_cfg, darknet_weights, darknet_names));
//		Log("neural network loaded in " + darkhelp().duration_string());
	}
	catch (const std::exception & e)
	{
		Log("failed to load darknet (cfg=" + darknet_cfg + ", weights=" + darknet_weights + ", names=" + darknet_names + "): " + e.what());
	}
	names = darkhelp().names;
	annotation_colours = darkhelp().annotation_colours;

	load_image(0);

	return;
}


void dm::DMContent::rebuild_image_and_repaint()
{
	canvas.need_to_rebuild_cache_image = true;
	canvas.repaint();

	for (auto c : corners)
	{
		c->need_to_rebuild_cache_image = true;
		c->repaint();
	}

	return;
}


bool dm::DMContent::keyPressed(const KeyPress &key)
{
//	Log("code=" + std::to_string(key.getKeyCode()) + " char=" + std::to_string(key.getTextCharacter()) + " description=" + key.getTextDescription().toStdString());

	const auto keycode = key.getKeyCode();

	const KeyPress key0 = KeyPress::createFromDescription("0");
	const KeyPress key9 = KeyPress::createFromDescription("9");

	int digit = -1;
	if (keycode >= key0.getKeyCode() and keycode <= key9.getKeyCode())
	{
		digit = keycode - key0.getKeyCode();
	}

	if (keycode == KeyPress::tabKey)
	{
		if (marks.empty())
		{
			selected_mark = -1;
		}
		else
		{
			if (key.getModifiers().isShiftDown())
			{
				// select previous mark
				selected_mark --;
				if (selected_mark < 0)
				{
					selected_mark = marks.size() - 1;
				}
			}
			else
			{
				// select next mark
				selected_mark ++;
				if (selected_mark >= (int)marks.size())
				{
					selected_mark = 0;
				}
			}

			rebuild_image_and_repaint();
			return true; // event has been handled
		}
	}
	else if (digit >= 0 and digit <= 9)
	{
		if (key.getModifiers().isCtrlDown())
		{
			digit += 10;
		}
		else if (key.getModifiers().isAltDown())
		{
			digit += 20;
		}

		// change the class for the selected mark
		set_class(digit);
		return true; // event has been handled
	}
	else if (keycode == KeyPress::homeKey)
	{
		load_image(0);
		return true;
	}
	else if (keycode == KeyPress::endKey)
	{
		load_image(image_filenames.size() - 1);
		return true;
	}
	else if (keycode == KeyPress::rightKey)
	{
		if (image_filename_index < image_filenames.size() - 1)
		{
			load_image(image_filename_index + 1);
		}
		return true;
	}
	else if (keycode == KeyPress::leftKey)
	{
		if (image_filename_index > 0)
		{
			load_image(image_filename_index - 1);
		}
		return true;
	}
	else if (keycode == KeyPress::pageUpKey)
	{
		// go to the previous available image with no marks
		while (image_filename_index > 0)
		{
			File f(image_filenames[image_filename_index]);
			f = f.withFileExtension(".json");
			if (count_marks_in_json(f) == 0)
			{
				break;
			}
			image_filename_index --;
		}
		load_image(image_filename_index);
		return true;

	}
	else if (keycode == KeyPress::pageDownKey)
	{
		// go to the next available image with no marks
		while (image_filename_index < image_filenames.size() - 1)
		{
			File f(image_filenames[image_filename_index]);
			f = f.withFileExtension(".json");
			if (count_marks_in_json(f) == 0)
			{
				break;
			}
			image_filename_index ++;
		}
		load_image(image_filename_index);
		return true;
	}
	else if (keycode == KeyPress::deleteKey or keycode == KeyPress::backspaceKey or keycode == KeyPress::numberPadDelete)
	{
		if (selected_mark >= 0)
		{
			auto iter = marks.begin() + selected_mark;
			marks.erase(iter);
			if (selected_mark >= (int)marks.size())
			{
				selected_mark --;
			}
			if (marks.empty())
			{
				selected_mark = -1;
			}
			need_to_save = true;
			rebuild_image_and_repaint();
			return true;
		}
	}
	else if (keycode == KeyPress::escapeKey)
	{
		dmapp().quit();
	}
	else if (key.getTextCharacter() == 'r')
	{
		set_sort_order(ESort::kRandom);
		return true;
	}
	else if (key.getTextCharacter() == 'a')
	{
		accept_all_marks();
		return true; // event has been handled
	}
	else if (key.getTextCharacter() == 'p')
	{
		show_predictions = not show_predictions;
		load_image(image_filename_index);
	}

	return false;
}


dm::DMContent & dm::DMContent::set_class(size_t class_idx)
{
	if (selected_mark >= 0 and (size_t)selected_mark < marks.size())
	{
		if (class_idx >= names.size())
		{
			Log("class idx \"" + std::to_string(class_idx) + "\" is beyond the last index");
			class_idx = names.size() - 1;
		}

		most_recent_class_idx = class_idx;

		auto & m = marks[selected_mark];
		m.class_idx = class_idx;
		m.name = names.at(m.class_idx);
		m.description = names.at(m.class_idx);
		need_to_save = true;
		rebuild_image_and_repaint();
	}

	return *this;
}


dm::DMContent & dm::DMContent::set_sort_order(const dm::ESort new_sort_order)
{
	if (sort_order != new_sort_order)
	{
		sort_order = new_sort_order;
		const int tmp = static_cast<int>(sort_order);

		Log("changing sort order to #" + std::to_string(tmp));
		cfg().setValue("sort_order", tmp);
	}

	switch (sort_order)
	{
		case ESort::kRandom:
		{
			std::random_shuffle(image_filenames.begin(), image_filenames.end());
			break;
		}
		case ESort::kCountMarks:
		{
			// this one takes a while, so start a progress thread to do the work
			DMContentImageFilenameSort helper(*this);
			helper.runThread();
			break;
		}
		case ESort::kTimestamp:
		{
			// this one takes a while, so start a progress thread to do the work
			DMContentImageFilenameSort helper(*this);
			helper.runThread();
			break;
		}
		case ESort::kAlphabetical:
		default:
		{
			std::sort(image_filenames.begin(), image_filenames.end());
			break;
		}
	}

	load_image(0);

	return *this;
}


dm::DMContent & dm::DMContent::set_labels(const EToggle toggle)
{
	if (show_labels != toggle)
	{
		show_labels = toggle;
		cfg().setValue("show_labels", static_cast<int>(show_labels));

		rebuild_image_and_repaint();
	}

	return *this;
}


dm::DMContent & dm::DMContent::load_image(const size_t new_idx)
{
	if (need_to_save)
	{
		save_json();
		save_text();
	}

	selected_mark	= -1;
	original_image	= cv::Mat();
	marks.clear();

	if (new_idx >= image_filenames.size())
	{
		image_filename_index = image_filenames.size() - 1;
	}
	else
	{
		image_filename_index = new_idx;
	}
	long_filename	= image_filenames.at(image_filename_index);
	short_filename	= File(long_filename).getFileName().toStdString();
	json_filename	= File(long_filename).withFileExtension(".json"	).getFullPathName().toStdString();
	text_filename	= File(long_filename).withFileExtension(".txt"	).getFullPathName().toStdString();

	try
	{
		Log("loading image " + long_filename);
		original_image = cv::imread(image_filenames.at(image_filename_index));

		bool success = load_json();
		if (not success)
		{
			success = load_text();
			if (success)
			{
				Log("imported " + text_filename + " instead of " + json_filename);
				need_to_save = true;
			}
		}

		if (not success or show_predictions)
		{
			if (dmapp().darkhelp)
			{
				darkhelp().predict(original_image);
				Log("darkhelp processed the image in " + darkhelp().duration_string());

				// convert the predictions into marks
				for (auto prediction : darkhelp().prediction_results)
				{
					Mark m(cv::Point2d(prediction.mid_x, prediction.mid_y), cv::Size2d(prediction.width, prediction.height), cv::Size(0, 0), prediction.best_class);
					m.name = names.at(m.class_idx);
					m.description = prediction.name;
					m.is_prediction = true;
					marks.push_back(m);
				}
			}
		}

		// Sort the marks based on a gross (rounded) X and Y position of the midpoint.  This way when
		// the user presses TAB or SHIFT+TAB the marks appear in a consistent and predictable order.
		std::sort(marks.begin(), marks.end(),
				  [](auto & lhs, auto & rhs)
				  {
					  const auto & p1 = lhs.get_normalized_midpoint();
					  const auto & p2 = rhs.get_normalized_midpoint();

					  const int y1 = std::round(15.0 * p1.y);
					  const int y2 = std::round(15.0 * p2.y);

					  if (y1 < y2) return true;
				  if (y2 < y1) return false;

				  // if we get here then y1 and y2 are the same, so now we compare x1 and x2

				  const int x1 = std::round(15.0 * p1.x);
				  const int x2 = std::round(15.0 * p2.x);

				  if (x1 < x2) return true;

				  return false;
				  } );
	}
	catch(const std::exception & e)
	{
		Log("Error: exception caught while loading " + long_filename + ": " + e.what());
	}
	catch(...)
	{
		Log("Error: failed to load image " + long_filename);
	}

	if (marks.size() > 0)
	{
		selected_mark = 0;
	}

	resized();
	rebuild_image_and_repaint();

	return *this;
}


dm::DMContent & dm::DMContent::save_text()
{
	if (text_filename.empty() == false)
	{
		std::ofstream fs(text_filename);
		for (const auto & m : marks)
		{
			const cv::Rect2d r	= m.get_normalized_bounding_rect();
			const double w		= r.width;
			const double h		= r.height;
			const double x		= r.x + w / 2.0;
			const double y		= r.y + h / 2.0;
			fs << std::fixed << std::setprecision(10) << m.class_idx << " " << x << " " << y << " " << w << " " << h << std::endl;
		}
	}

	return *this;
}


dm::DMContent & dm::DMContent::save_json()
{
	if (json_filename.empty() == false)
	{
		json root;
		size_t next_id = 0;
		for (const auto & m : marks)
		{
			if (m.is_prediction)
			{
				// skip this one since it is a prediction, not a full mark
				continue;
			}

			root["mark"][next_id]["class_idx"	] = m.class_idx;
			root["mark"][next_id]["name"		] = m.name;
			for (size_t point_idx = 0; point_idx < m.normalized_all_points.size(); point_idx ++)
			{
				const cv::Point2d & p = m.normalized_all_points.at(point_idx);
				root["mark"][next_id]["points"][point_idx]["x"] = p.x;
				root["mark"][next_id]["points"][point_idx]["y"] = p.y;

				// DarkMark doesn't use these integer values, but make them available for 3rd party software which wants to reads the .json file
				root["mark"][next_id]["points"][point_idx]["int_x"] = (int)(std::round(p.x * (double)original_image.cols));
				root["mark"][next_id]["points"][point_idx]["int_y"] = (int)(std::round(p.y * (double)original_image.rows));
			}

			next_id ++;
		}
		root["image"]["scale"]	= scale_factor;
		root["image"]["width"]	= original_image.cols;
		root["image"]["height"]	= original_image.rows;
		root["timestamp"]		= std::time(nullptr);
		root["version"]			= DARKMARK_VERSION;

		std::ofstream fs(json_filename);
		fs << root.dump(1, '\t') << std::endl;
	}

	need_to_save = false;

	return *this;
}


size_t dm::DMContent::count_marks_in_json(File & f)
{
	size_t result = 0;

	if (f.existsAsFile())
	{
		try
		{
			json root = json::parse(f.loadFileAsString().toStdString());
			result = root["mark"].size();
		}
		catch (const std::exception & e)
		{
			AlertWindow::showMessageBox(
				AlertWindow::AlertIconType::InfoIcon,
				"DarkMark",
				"Failed to read or parse the .json file " + f.getFullPathName().toStdString() + ":\n"
				"\n" +
				e.what());
		}
	}

	return result;
}


bool dm::DMContent::load_text()
{
	bool success = false;

	File f(text_filename);
	if (f.existsAsFile())
	{
		success = true;
		StringArray sa;
		f.readLines(sa);
		sa.removeEmptyStrings();
		for (auto iter = sa.begin(); iter != sa.end(); iter ++)
		{
			std::stringstream ss(iter->toStdString());
			int class_idx = 0;
			double x = 0.0;
			double y = 0.0;
			double w = 0.0;
			double h = 0.0;
			ss >> class_idx >> x >> y >> w >> h;
			Mark m(cv::Point2d(x, y), cv::Size2d(w, h), cv::Size(0, 0), class_idx);
			m.name = names.at(class_idx);
			m.description = m.name;
			marks.push_back(m);
		}
	}

	return success;
}


bool dm::DMContent::load_json()
{
	bool success = false;

	File f(json_filename);
	if (f.existsAsFile())
	{
		json root = json::parse(f.loadFileAsString().toStdString());

		for (size_t idx = 0; idx < root["mark"].size(); idx ++)
		{
			Mark m;
			m.class_idx = root["mark"][idx]["class_idx"];
			m.name = root["mark"][idx]["name"];
			m.description = m.name;
			m.normalized_all_points.clear();
			for (size_t point_idx = 0; point_idx < root["mark"][idx]["points"].size(); point_idx ++)
			{
				cv::Point2d p;
				p.x = root["mark"][idx]["points"][point_idx]["x"];
				p.y = root["mark"][idx]["points"][point_idx]["y"];
				m.normalized_all_points.push_back(p);
			}
			m.rebalance();
			marks.push_back(m);
		}

		success = true;
	}

	return success;
}


dm::DMContent & dm::DMContent::create_darknet_files()
{
	const std::string output_dir		= image_directory.getFullPathName().toStdString();
	const std::string project_name		= image_directory.getFileNameWithoutExtension().toStdString();
	const std::string data_filename		= image_directory.getChildFile(project_name				).withFileExtension(".data"	).getFullPathName().toStdString();
	const std::string valid_filename	= image_directory.getChildFile(project_name + "_valid"	).withFileExtension(".txt"	).getFullPathName().toStdString();
	const std::string train_filename	= image_directory.getChildFile(project_name + "_train"	).withFileExtension(".txt"	).getFullPathName().toStdString();
	const std::string command_filename	= image_directory.getChildFile(project_name + "_train"	).withFileExtension(".sh"	).getFullPathName().toStdString();

	if (true)
	{
		std::ofstream fs(data_filename);
		fs	<< "classes = " << names.size()						<< std::endl
			<< "train = " << train_filename						<< std::endl
			<< "valid = " << valid_filename						<< std::endl
			<< "names = " << cfg().get_str("darknet_names")		<< std::endl
			<< "backup = " << output_dir						<< std::endl;
	}

	size_t number_of_files_train = 0;
	size_t number_of_files_valid = 0;
	size_t number_of_skipped_files = 0;
	size_t number_of_marks = 0;
	if (true)
	{
		const double percentage_of_image_files_to_use_for_training = 0.85;

		// only include the images for which we have at least 1 mark
		VStr v;
		for (const auto & filename : image_filenames)
		{
			File f = File(filename).withFileExtension(".json");
			const size_t count = count_marks_in_json(f);
			if (count == 0)
			{
				number_of_skipped_files ++;
			}
			else
			{
				number_of_marks += count;
				v.push_back(filename);
			}
		}

		std::random_shuffle(v.begin(), v.end());
		number_of_files_train = std::round(percentage_of_image_files_to_use_for_training * v.size());
		number_of_files_valid = v.size() - number_of_files_train;

		std::ofstream fs_train(train_filename);
		std::ofstream fs_valid(valid_filename);

		for (size_t idx = 0; idx < v.size(); idx ++)
		{
			if (idx < number_of_files_train)
			{
				fs_train << v[idx] << std::endl;
			}
			else
			{
				fs_valid << v[idx] << std::endl;
			}
		}
	}

	if (true)
	{
		std::stringstream ss;
		ss	<< "#!/bin/bash"				<< std::endl
			<< ""							<< std::endl
			<< "cd " << output_dir			<< std::endl
			<< ""							<< std::endl
			<< "# other parms:  -show_imgs"	<< std::endl
			<< "/usr/bin/time --verbose ~/darknet/darknet detector -map -mjpeg_port 8090 -dont_show train " << data_filename << " " << project_name << "_yolov3-tiny.cfg" << std::endl
			<< ""							<< std::endl;
		const std::string data = ss.str();
		File f(command_filename);
		f.replaceWithData(data.c_str(), data.size());	// do not use replaceWithText() since it converts the file to CRLF endings which confuses bash
		f.setExecutePermission(true);
	}

	if (true)
	{
		std::stringstream ss;
		ss	<< "The necessary files to run darknet have been saved to " << output_dir << "." << std::endl
			<< std::endl
			<< "There are " << names.size() << " classes with a total of "
			<< number_of_files_train << " training files and "
			<< number_of_files_valid << " validation files. The average is "
			<< std::fixed << std::setprecision(2) << double(number_of_marks) / double(number_of_files_train + number_of_files_valid)
			<< " marks per image." << std::endl
			<< std::endl;

		if (number_of_skipped_files)
		{
			ss	<< "IMPORTANT: " << number_of_skipped_files << " images were skipped because they have not yet been marked." << std::endl
				<< std::endl;
		}

		ss << "Run " << command_filename << " to start the training.";

		AlertWindow::showMessageBox(AlertWindow::AlertIconType::InfoIcon, "DarkMark", ss.str());
	}

	return *this;
}


dm::DMContent & dm::DMContent::delete_current_image()
{
	if (image_filename_index < image_filenames.size())
	{
		File f(image_filenames[image_filename_index]);
		Log("deleting the file at index #" + std::to_string(image_filename_index) + ": " + f.getFullPathName().toStdString());
		f.deleteFile();
		f.withFileExtension(".txt"	).deleteFile();
		f.withFileExtension(".json"	).deleteFile();
		image_filenames.erase(image_filenames.begin() + image_filename_index);
		load_image(image_filename_index);
	}

	return *this;
}


dm::DMContent & dm::DMContent::accept_all_marks()
{
	for (auto & m : marks)
	{
		m.is_prediction	= false;
		m.name			= names.at(m.class_idx);
		m.description	= names.at(m.class_idx);
	}

	need_to_save	= true;
	rebuild_image_and_repaint();

	return *this;
}


dm::DMContent & dm::DMContent::erase_all_marks()
{
	Log("deleting all marks for " + long_filename);

	marks.clear();
	need_to_save = false;
	File(json_filename).deleteFile();
	File(text_filename).deleteFile();
	load_image(image_filename_index);
	rebuild_image_and_repaint();

	return *this;
}


PopupMenu dm::DMContent::create_class_menu()
{
	const bool is_enabled = (selected_mark >= 0 and (size_t)selected_mark < marks.size() ? true : false);

	int selected_class_idx = -1;
	if (is_enabled)
	{
		const Mark & m = marks.at(selected_mark);
		selected_class_idx = (int)m.class_idx;
	}

	PopupMenu m;
	for (size_t idx = 0; idx < names.size(); idx ++)
	{
		const std::string & name = names.at(idx);

		const bool is_ticked = (selected_class_idx == (int)idx ? true : false);

		m.addItem(name, (is_enabled and not is_ticked), is_ticked, std::function<void()>( [&]{ this->set_class(idx); } ));
	}

	return m;
}


PopupMenu dm::DMContent::create_popup_menu()
{
	PopupMenu classMenu = create_class_menu();

	PopupMenu labels;
	labels.addItem("always show labels"	, (show_labels != EToggle::kOn	), (show_labels == EToggle::kOn		), std::function<void()>( [&]{ set_labels(EToggle::kOn);	} ));
	labels.addItem("never show labels"	, (show_labels != EToggle::kOff	), (show_labels == EToggle::kOff	), std::function<void()>( [&]{ set_labels(EToggle::kOff);	} ));
	labels.addItem("auto show labels"	, (show_labels != EToggle::kAuto), (show_labels == EToggle::kAuto	), std::function<void()>( [&]{ set_labels(EToggle::kAuto);	} ));

	PopupMenu sort;
	sort.addItem("sort alphabetically"								, true, (sort_order == ESort::kAlphabetical	), std::function<void()>( [&]{ set_sort_order(ESort::kAlphabetical	); } ));
	sort.addItem("sort by modification timestamp"					, true, (sort_order == ESort::kTimestamp	), std::function<void()>( [&]{ set_sort_order(ESort::kTimestamp		); } ));
	sort.addItem("sort by number of marks"							, true, (sort_order == ESort::kCountMarks	), std::function<void()>( [&]{ set_sort_order(ESort::kCountMarks	); } ));
	sort.addItem("sort randomly"									, true, (sort_order == ESort::kRandom		), std::function<void()>( [&]{ set_sort_order(ESort::kRandom		); } ));

	const size_t number_of_darknet_marks = [&]
	{
		size_t count = 0;
		for (const auto & m : marks)
		{
			if (m.is_prediction)
			{
				count ++;
			}
		}

		return count;
	}();

	const bool has_any_marks = (marks.size() > 0);

	PopupMenu image;
	image.addItem("accept " + std::to_string(number_of_darknet_marks) + " pending marks", (number_of_darknet_marks > 0)	, false	, std::function<void()>( [&]{ accept_all_marks();		} ));
	image.addItem("erase all " + std::to_string(marks.size()) + " marks"		, has_any_marks							, false	, std::function<void()>( [&]{ erase_all_marks();		} ));
	image.addItem("delete image from disk"																						, std::function<void()>( [&]{ delete_current_image();	} ));

	PopupMenu m;
	m.addSubMenu("class", classMenu, classMenu.containsAnyActiveItems());
	m.addSubMenu("labels", labels);
	m.addSubMenu("sort", sort);
	m.addSubMenu("image", image);
	m.addSeparator();
	m.addItem("create darknet files", std::function<void()>( [&]{ create_darknet_files(); } ));

	return m;
}
