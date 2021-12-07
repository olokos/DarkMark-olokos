// DarkMark (C) 2019-2021 Stephane Charette <stephanecharette@gmail.com>

#include "DarkMark.hpp"

#include "json.hpp"
using json = nlohmann::json;


dm::DMContent::DMContent(const std::string & prefix) :
	cfg_prefix(prefix),
	show_window(not (dmapp().cli_options.count("editor") and dmapp().cli_options.at("editor") == "gen-darknet")),
	canvas(*this),
	scrollfield(*this),
	scrollfield_width(cfg().get_int("scrollfield_width")),
	highlight_x(0.0f),
	highlight_y(0.0f),
	highlight_inside_size(-1.0f),
	highlight_outside_size(-1.0f),
	empty_image_name_index(0),
	sort_order(static_cast<ESort>(cfg().get_int("sort_order"))),
	show_labels(static_cast<EToggle>(cfg().get_int("show_labels"))),
	show_predictions(static_cast<EToggle>(cfg().get_int("show_predictions"))),
	image_is_completely_empty(false),
	show_marks(cfg().get_bool("show_marks")),
	marks_are_shown(false),
	predictions_are_shown(false),
	number_of_marks(0),
	number_of_predictions(0),
	alpha_blend_percentage(static_cast<double>(cfg().get_int("alpha_blend_percentage")) / 100.0),
	shade_rectangles(cfg().get_bool("shade_rectangles")),
	all_marks_are_bold(cfg().get_bool("all_marks_are_bold")),
	show_processing_time(cfg().get_bool("show_processing_time")),
	need_to_save(false),
	show_mouse_pointer(cfg().get_bool("show_mouse_pointer")),
	corner_size(cfg().get_int("corner_size")),
	selected_mark(-1),
	images_are_loading(false),
	scale_factor(1.0),
	most_recent_class_idx(0),
	image_filename_index(0),
	project_info(cfg_prefix),
	user_specified_zoom_factor(-1.0),
	previous_zoom_factor(5.0),
	current_zoom_factor(1.0)
{
	addAndMakeVisible(canvas);
	addAndMakeVisible(scrollfield);

	addAndMakeVisible(bubble_message);
	bubble_message.toFront(false);

	crosshair_colour = Colours::white;

	setWantsKeyboardFocus(true);

	VStr json_filenames;
	images_without_json.clear();
	std::atomic<bool> done = false;
	find_files(File(project_info.project_dir), image_filenames, json_filenames, images_without_json, done);
	Log("number of images found in " + project_info.project_dir + ": " + std::to_string(image_filenames.size()));

	const auto & action = dmapp().cli_options["editor"];

	try
	{
		const std::string inclusion_regex = cfg().get_str(cfg_prefix + "inclusion_regex");
		const std::string exclusion_regex = cfg().get_str(cfg_prefix + "exclusion_regex");

		const std::regex rx(inclusion_regex + exclusion_regex);

		VStr v;
		for (auto && fn : image_filenames)
		{
			const bool result = std::regex_search(fn, rx);
			if (result == exclusion_regex.empty())
			{
				v.push_back(fn);
			}
		}

		if (v.size() != image_filenames.size())
		{
			v.swap(image_filenames);

			if (action != "gen-darknet")
			{
				AlertWindow::showMessageBoxAsync(AlertWindow::AlertIconType::InfoIcon, "DarkMark",
						"This project has a regex filter:\n\n"
						"\t\t" + inclusion_regex + exclusion_regex + "\n\n" +
						std::to_string(v.size() - image_filenames.size()) + " images were excluded by this filter, bringing the total number of images from " +
						std::to_string(v.size()) + " down to " + std::to_string(image_filenames.size()) + ".\n\n"
						"Clear the \"inclusion regex\" and \"exclusion regex\" fields in the launcher window to include all images in the project."
						);
			}
		}
	}
	catch (...)
	{
		AlertWindow::showMessageBoxAsync(AlertWindow::AlertIconType::WarningIcon, "DarkMark", "The \"inclusion regex\" or \"exclusion regex\" for this project has caused an error and has been skipped.");
	}

	if (images_without_json.empty() == false and action != "gen-darknet")
	{
		String msg = "1 image file was found with \".txt\" annotations.";
		if (images_without_json.size() > 1)
		{
			msg = String(images_without_json.size()) + " image files were found with \".txt\" annotations.";
		}
		Log(msg.toStdString());

		msg += "\n\nWould you like to import the annotations into DarkMark?";

		AlertWindow::showOkCancelBox(AlertWindow::AlertIconType::QuestionIcon, "DarkMark", msg, "", "", nullptr,
				ModalCallbackFunction::create(
					[=](int result)
					{
						if (result == 1)
						{
							this->import_text_annotations(images_without_json);
						}
					}));
	}

	if (image_filenames.empty())
	{
		// projects without images shouldn't be allowed to load,
		// but just in case this happens insert a dummy image so the vector isn't empty
		image_filenames.push_back(project_info.project_dir + "/no_image_found.png");
	}

	return;
}


dm::DMContent::~DMContent(void)
{
	stopTimer();

	if (need_to_save)
	{
		save_json();
		save_text();
	}

	return;
}


void dm::DMContent::resized()
{
	if (dmapp().wnd == nullptr)
	{
		// window has not yet been fully created
		return;
	}

	if (show_window == false)
	{
		dmapp().wnd->setMinimised(true);
		dmapp().wnd->setVisible(false);
		return;
	}

	const double window_width	= getWidth();
	const double window_height	= getHeight();
	if(	window_width	< 1.0 or
		window_height	< 1.0 )
	{
		// window hasn't been created yet?
		return;
	}

	double image_width	= original_image.cols;
	double image_height	= original_image.rows;
	if (image_width		< 1.0 or
		image_height	< 1.0 )
	{
		// image hasn't been loaded yet?
		image_width		= 640;
		image_height	= 480;
	}

	// determine the size of the image once it is scaled
	const double min_horizontal_spacer_height	= (scrollfield_width > 0 ? 2.0 : 0.0);
	const double width_ratio					= (window_width - min_horizontal_spacer_height - scrollfield_width) / image_width;
	const double height_ratio					= window_height / image_height;
	double ratio								= std::min(width_ratio, height_ratio);

	double new_image_width	= std::round(ratio * image_width);
	double new_image_height	= std::round(ratio * image_height);

	int canvas_width = new_image_width;
	int canvas_height = new_image_height;

	// ...but if we're zooming by a user-defined amount, then we need to alter a few of these values
	if (user_specified_zoom_factor > 0.0)
	{
		ratio = user_specified_zoom_factor;
		new_image_width		= std::round(ratio * image_width);
		new_image_height	= std::round(ratio * image_height);

		canvas_width = window_width - min_horizontal_spacer_height - scrollfield_width;
		canvas_height = window_height;

		if (new_image_width		< canvas_width	) canvas_width	= new_image_width;
		if (new_image_height	< canvas_height	) canvas_height	= new_image_height;
	}

	canvas.setBounds(0, 0, canvas_width, canvas_height);
	scrollfield.setBounds(window_width - scrollfield_width, 0, scrollfield_width, window_height);

	// remember some of the important numbers so we don't have to re-calculate them later
	scaled_image_size		= cv::Size(new_image_width, new_image_height);
	current_zoom_factor		= ratio;
	scale_factor			= ratio;

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
			" @ "	+ std::to_string(static_cast<int>(std::round(scale_factor * 100.0))) + "%";

		if (ratio > 0.0 and ratio != 1.0)
		{
			title += " = " +
				std::to_string(static_cast<int>(std::round(new_image_width))) +
				"x" +
				std::to_string(static_cast<int>(std::round(new_image_height)));
		}

		dmapp().wnd->setName(title);
	}

	return;
}


void dm::DMContent::start_darknet()
{
	Log("loading darknet neural network");
	const std::string darknet_cfg		= cfg().get_str(cfg_prefix + "cfg"		);
	const std::string darknet_weights	= cfg().get_str(cfg_prefix + "weights"	);
	const std::string darknet_names		= cfg().get_str(cfg_prefix + "names"	);
	names.clear();

	if (darknet_cfg		.empty() == false	and
		darknet_weights	.empty() == false	and
		File(darknet_cfg).existsAsFile()	and
		File(darknet_weights).existsAsFile())
	{
		try
		{
			Log("attempting to load neural network " + darknet_cfg + " / " + darknet_weights + " / " + darknet_names);
			dmapp().darkhelp_nn.reset(new DarkHelp::NN(darknet_cfg, darknet_weights, darknet_names));
			Log("neural network loaded in " + darkhelp_nn().duration_string());

			darkhelp_nn().config.threshold							= cfg().get_int("darknet_threshold")			/ 100.0f;
			darkhelp_nn().config.hierarchy_threshold				= cfg().get_int("darknet_hierarchy_threshold")	/ 100.0f;
			darkhelp_nn().config.non_maximal_suppression_threshold	= cfg().get_int("darknet_nms_threshold")		/ 100.0f;
			darkhelp_nn().config.enable_tiles						= cfg().get_bool("darknet_image_tiling");
			names = darkhelp_nn().names;
		}
		catch (const std::exception & e)
		{
			dmapp().darkhelp_nn.reset(nullptr);
			Log("failed to load darknet (cfg=" + darknet_cfg + ", weights=" + darknet_weights + ", names=" + darknet_names + "): " + e.what());
			if (show_window)
			{
				AlertWindow::showMessageBoxAsync(
					AlertWindow::AlertIconType::WarningIcon,
					"DarkMark",
					"Failed to load darknet neural network. The error message returned was:\n" +
					String("\n") +
					e.what());
			}
		}
	}
	else
	{
		Log("skipped loading darknet due to missing or invalid .cfg or .weights filenames");
#if 0
		if (show_window)
		{
			AlertWindow::showMessageBoxAsync(
				AlertWindow::AlertIconType::InfoIcon,
				"DarkMark",
				"One or more required neural network file was not found. The neural network cannot be loaded.");
		}
#endif
	}

	if (names.empty() and darknet_names.empty() == false)
	{
		Log("manually parsing " + darknet_names);
		std::ifstream ifs(darknet_names);
		std::string line;
		while (std::getline(ifs, line))
		{
			if (line.empty())
			{
				break;
			}
			names.push_back(line);
		}
	}
	if (names.empty())
	{
		Log("classes/names is empty -- creating some dummy entries");
		names = { "car", "person", "bicycle", "dog", "cat" };
	}

	Log("number of name entries: " + std::to_string(names.size()));

	// add 1 more special entry to the end of the "names" so we can deal with empty images
	empty_image_name_index = names.size();
	names.push_back("* empty image *");

	annotation_colours = DarkHelp::get_default_annotation_colours();
	if (annotation_colours.empty() == false)
	{
		const auto & opencv_colour = annotation_colours.at(most_recent_class_idx % annotation_colours.size());
		crosshair_colour = Colour(opencv_colour[2], opencv_colour[1], opencv_colour[0]);
	}

	set_sort_order(sort_order);

	return;
}


void dm::DMContent::paintOverChildren(Graphics & g)
{
	if (highlight_inside_size > 0.0f)
	{
		// draw black+white circles on top of the image

		g.setOpacity(0.5f);
		g.setColour(Colours::black);
		g.drawEllipse(highlight_x - highlight_outside_size/2, highlight_y - highlight_outside_size/2, highlight_outside_size, highlight_outside_size, 5);
		g.setColour(Colours::white);
		g.drawEllipse(highlight_x - highlight_inside_size/2, highlight_y - highlight_inside_size/2, highlight_inside_size, highlight_inside_size, 5);

		highlight_outside_size = highlight_inside_size;
		highlight_inside_size -= 10.0f;
	}

	return;
}


void dm::DMContent::rebuild_image_and_repaint()
{
	canvas.need_to_rebuild_cache_image = true;
	canvas.repaint();

	if (scrollfield_width > 0)
	{
		scrollfield.draw_marker_at_current_image();
	}

	return;
}


bool dm::DMContent::keyPressed(const KeyPress & key)
{
//	Log("code=" + std::to_string(key.getKeyCode()) + " char=" + std::to_string(key.getTextCharacter()) + " description=" + key.getTextDescription().toStdString());

	const auto keycode = key.getKeyCode();
	const auto keychar = key.getTextCharacter();
//	show_message(key.getTextDescription().toStdString());

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
			int attempt = 10;
			while (attempt >= 0)
			{
				if (key.getModifiers().isShiftDown())
				{
					// select previous mark
					selected_mark --;
					if (selected_mark < 0 or selected_mark >= (int)marks.size())
					{
						// wrap back around to the last mark
						selected_mark = marks.size() - 1;
					}
				}
				else
				{
					// select next mark
					selected_mark ++;
					if (selected_mark < 0 or selected_mark >= (int)marks.size())
					{
						// wrap back around to the very first mark
						selected_mark = 0;
					}
				}

				const auto & m = marks.at(selected_mark);
				if ((marks_are_shown and m.is_prediction == false) or
					(predictions_are_shown and m.is_prediction))
				{
					// we found one that works!  keep it!
					break;
				}

				// try again to find a mark that is shown on the screen
				attempt --;
			}
			if (attempt < 0)
			{
				selected_mark = -1;
			}
		}

		if (selected_mark >= 0)
		{
			// remember the class and size of this mark in case the user wants to double-click and create a similar one
			const Mark & m = marks.at(selected_mark);
			most_recent_class_idx = m.class_idx;
			most_recent_size = m.get_normalized_bounding_rect().size();

			const auto & opencv_colour = annotation_colours.at(most_recent_class_idx % annotation_colours.size());
			crosshair_colour = Colour(opencv_colour[2], opencv_colour[1], opencv_colour[0]);
		}

		rebuild_image_and_repaint();
		return true; // event has been handled
	}
	else if (digit >= 0 and digit <= 9)
	{
		if (key.getModifiers().isCtrlDown())
		{
			digit += 10;
		}
		if (key.getModifiers().isAltDown())
		{
			digit += 20;
		}

		// change the class for the selected mark
		set_class(digit);
		return true; // event has been handled
	}
	else if (keycode == KeyPress::homeKey)
	{
		if (user_specified_zoom_factor > 0.0)
		{
			// jump out of "zoom" mode before we do anything else
			keyPressed(KeyPress::createFromDescription("spacebar"));
		}
		load_image(0);
		return true;
	}
	else if (keycode == KeyPress::endKey)
	{
		if (user_specified_zoom_factor > 0.0)
		{
			// jump out of "zoom" mode before we do anything else
			keyPressed(KeyPress::createFromDescription("spacebar"));
		}
		load_image(image_filenames.size() - 1);
		return true;
	}
	else if (keycode == KeyPress::rightKey)
	{
		if (user_specified_zoom_factor > 0.0)
		{
			// jump out of "zoom" mode before we do anything else
			keyPressed(KeyPress::createFromDescription("spacebar"));
		}
		if (image_filename_index < image_filenames.size() - 1)
		{
			load_image(image_filename_index + 1);
		}
		return true;
	}
	else if (keycode == KeyPress::leftKey)
	{
		if (user_specified_zoom_factor > 0.0)
		{
			// jump out of "zoom" mode before we do anything else
			keyPressed(KeyPress::createFromDescription("spacebar"));
		}
		if (image_filename_index > 0)
		{
			load_image(image_filename_index - 1);
		}
		return true;
	}
	else if (keycode == KeyPress::pageUpKey)
	{
		if (user_specified_zoom_factor > 0.0)
		{
			// jump out of "zoom" mode before we do anything else
			keyPressed(KeyPress::createFromDescription("spacebar"));
		}

		// go to the previous available image with no marks
		auto idx = image_filename_index;
		while (idx > 0)
		{
			idx --;

			File f(image_filenames[idx]);
			f = f.withFileExtension(".json");
			if (count_marks_in_json(f) == 0)
			{
				break;
			}
		}
		load_image(idx);
		return true;

	}
	else if (keycode == KeyPress::pageDownKey)
	{
		if (user_specified_zoom_factor > 0.0)
		{
			// jump out of "zoom" mode before we do anything else
			keyPressed(KeyPress::createFromDescription("spacebar"));
		}

		// go to the next available image with no marks
		auto idx = image_filename_index;
		while (idx < image_filenames.size() - 1)
		{
			idx ++;

			File f(image_filenames[idx]);
			f = f.withFileExtension(".json");
			if (count_marks_in_json(f) == 0)
			{
				break;
			}
		}
		load_image(idx);
		return true;
	}
	else if (keycode == KeyPress::upKey or keycode == KeyPress::downKey)
	{
		if (dmapp().darkhelp_nn)
		{
			float threshold = dmapp().darkhelp_nn->config.threshold;

			threshold += (keycode == KeyPress::upKey ? 0.05f : -0.05f);
			threshold = std::min(std::max(threshold, 0.05f), 0.95f);

			if (threshold != dmapp().darkhelp_nn->config.threshold)
			{
				dmapp().darkhelp_nn->config.threshold = threshold;
				load_image(image_filename_index);
				show_message("darknet threshold: " + std::to_string((int)std::round(100.0 * threshold)) + "%");
			}
		}
		return true;
	}
	else if (keycode == KeyPress::deleteKey and key.getModifiers().isShiftDown())
	{
		if (user_specified_zoom_factor > 0.0)
		{
			// jump out of "zoom" mode before we do anything else
			keyPressed(KeyPress::createFromDescription("spacebar"));
		}
		delete_current_image();
		return true;
	}
	else if (keycode == KeyPress::deleteKey or keycode == KeyPress::backspaceKey or keycode == KeyPress::numberPadDelete)
	{
		if (selected_mark >= 0)
		{
			auto iter = marks.begin() + selected_mark;
			marks.erase(iter);
			selected_mark = -1;
			need_to_save = true;
			rebuild_image_and_repaint();
			return true;
		}
	}
	else if (keycode == KeyPress::escapeKey)
	{
		if (user_specified_zoom_factor > 0.0)
		{
			// get out of zoom mode instead of quitting from the application
			return keyPressed(KeyPress::createFromDescription("spacebar"));
		}

		dmapp().wnd->closeButtonPressed();
		return true;
	}
	else if (keycode == KeyPress::F1Key)
	{
		if (not dmapp().about_wnd)
		{
			dmapp().about_wnd.reset(new AboutWnd);
		}
		dmapp().about_wnd->toFront(true);
		return true;
	}
	else if (keychar == '-') // why is the value for KeyPress::numberPadSubtract unusable!?
	{
		if (user_specified_zoom_factor <= 0.0)
		{
			user_specified_zoom_factor = std::ceil(current_zoom_factor * 10.0) / 10.0;
		}
		user_specified_zoom_factor -= 0.1;

		// use rounded zoom numbers -- so 0.19999 gets rounded to 0.2
		user_specified_zoom_factor = std::round(user_specified_zoom_factor * 10.0) / 10.0;

		if (user_specified_zoom_factor < 0.01)
		{
			user_specified_zoom_factor = 0.01;
		}

		resized();
		rebuild_image_and_repaint();
		show_message("zoom: " + std::to_string(static_cast<int>(user_specified_zoom_factor * 100.0)) + "%");
		return true;
	}
	else if (keychar == '+') // why is the value for KeyPress::numberPadAdd unusable!?
	{
		Log("zoom factor was: " + std::to_string(current_zoom_factor));
		Log("zoom point of interest was: x=" + std::to_string(zoom_point_of_interest.x) + " y=" + std::to_string(zoom_point_of_interest.y));
		const auto point = canvas.getLocalPoint(nullptr, Desktop::getMousePosition());
		Log("zoom mouse location now is: x=" + std::to_string(point.x) + " y=" + std::to_string(point.y));
		zoom_point_of_interest.x = std::round((point.x + canvas.zoom_image_offset.x) / current_zoom_factor);
		zoom_point_of_interest.y = std::round((point.y + canvas.zoom_image_offset.y) / current_zoom_factor);
		Log("zoom point of interest now: x=" + std::to_string(zoom_point_of_interest.x) + " y=" + std::to_string(zoom_point_of_interest.y));

		user_specified_zoom_factor = std::round(current_zoom_factor * 10.0 + 1.0) / 10.0;
		if (user_specified_zoom_factor > 5.0)
		{
			user_specified_zoom_factor = 5.0;
		}
		Log("zoom factor now: " + std::to_string(user_specified_zoom_factor));

		resized();
		rebuild_image_and_repaint();
		show_message("zoom: " + std::to_string(static_cast<int>(user_specified_zoom_factor * 100.0)) + "%");
		return true;
	}
	else if (keycode == KeyPress::spaceKey)
	{
		if (user_specified_zoom_factor > 0.0)
		{
			// go back to "auto" zoom
			zoom_point_of_interest = cv::Size(0, 0);
			previous_zoom_factor = user_specified_zoom_factor;
			user_specified_zoom_factor = -1.0;
			show_message("zoom: auto");
		}
		else
		{
			// restore the previous zoom factor around the current mouse position

			const auto point = canvas.getLocalPoint(nullptr, Desktop::getMousePosition());
			Log("zoom point of interest was: x=" + std::to_string(point.x) + " y=" + std::to_string(point.y));
			zoom_point_of_interest.x = std::max(0, point.x) / current_zoom_factor;
			zoom_point_of_interest.y = std::max(0, point.y) / current_zoom_factor;
			Log("zoom point of interest now: x=" + std::to_string(zoom_point_of_interest.x) + " y=" + std::to_string(zoom_point_of_interest.y));
			user_specified_zoom_factor = previous_zoom_factor;
			show_message("zoom: " + std::to_string(static_cast<int>(user_specified_zoom_factor * 100.0)) + "%");
		}

		resized();
		rebuild_image_and_repaint();
		return true;
	}
	else if (keychar == 'r')
	{
		if (user_specified_zoom_factor > 0.0)
		{
			// jump out of "zoom" mode before we do anything else
			keyPressed(KeyPress::createFromDescription("spacebar"));
		}
		set_sort_order(ESort::kRandom);
		show_message("re-shuffle random sort");
		return true;
	}
	else if (keychar == 'R')
	{
		if (user_specified_zoom_factor > 0.0)
		{
			// jump out of "zoom" mode before we do anything else
			keyPressed(KeyPress::createFromDescription("spacebar"));
		}
		set_sort_order(ESort::kAlphabetical);
		show_message("alphabetical sort");
		return true;
	}
	else if (keychar == 'a')
	{
		accept_all_marks();
		return true; // event has been handled
	}
	else if (keychar == 'A')
	{
		accept_current_mark();
		return true; // event has been handled
	}
	else if (keychar == 'p')
	{
		EToggle toggle = static_cast<EToggle>( (int(show_predictions) + 1) % 3 );
		toggle_show_predictions(toggle);
		show_message("predictions: " + std::string(
				toggle == EToggle::kOn	? "on"	:
				toggle == EToggle::kOff	? "off"	: "auto"));

		return true;
	}
	else if (keychar == 'm')
	{
		toggle_show_marks();
		show_message("user marks: " + std::string(show_marks ? "visible" : "hidden"));
		return true;
	}
	else if (keychar == 'l')
	{
		EToggle toggle = static_cast<EToggle>( (int(show_labels) + 1) % 3 );
		set_labels(toggle);
		show_message("labels: " + std::string(
				toggle == EToggle::kOn	? "on"	:
				toggle == EToggle::kOff	? "off"	: "auto"));
		return true;
	}
	else if (keychar == 'b')
	{
		toggle_bold_labels();
		show_message("bold: " + std::string(all_marks_are_bold ? "enable" : "disable"));
		return true;
	}
	else if (keychar == 'B')
	{
		toggle_shade_rectangles();
		show_message("shade: " + std::string(shade_rectangles ? "enable" : "disable"));
		return true;
	}
	else if (keychar == 'C')
	{
		erase_all_marks();
		return true;
	}
	else if (keychar == 'c' or keycode == KeyPress::returnKey)
	{
		create_class_menu().showMenuAsync(PopupMenu::Options());
		return true;
	}
	else if (keychar == 'j')
	{
		show_jump_wnd();
		return true;
	}
	else if (keychar == 'n')
	{
		if (number_of_marks == 0)
		{
			if (user_specified_zoom_factor > 0.0)
			{
				// jump out of "zoom" mode before we do anything else
				keyPressed(KeyPress::createFromDescription("spacebar"));
			}
			image_is_completely_empty = true;
			need_to_save = true;

			// pretend as if PAGEDOWN was pressed so we move to the next image
			return keyPressed(KeyPress::createFromDescription("page down"));
		}

		show_message("delete annotations before marking the image as empty");
		return true;
	}
	else if (keychar == 's')
	{
		save_screenshot(false);
		return true;
	}
	else if (keychar == 'S')
	{
		save_screenshot(true);
		return true;
	}
	else if (keychar == 't')
	{
		if (dmapp().darkhelp_nn)
		{
			dmapp().darkhelp_nn->config.enable_tiles = ! dmapp().darkhelp_nn->config.enable_tiles;
			show_message("image tiling: " + std::string(dmapp().darkhelp_nn->config.enable_tiles ? "enable" : "disable"));
			load_image(image_filename_index);
			cfg().setValue("darknet_image_tiling", dmapp().darkhelp_nn->config.enable_tiles);
		}
		return true;
	}
	else if (keychar == 'y')
	{
		copy_marks_from_previous_image();
		return true;
	}
	else if (keychar == 'Y')
	{
		copy_marks_from_next_image();
		return true;
	}
	else if (keychar == 'e')
	{
		if (not dmapp().settings_wnd)
		{
			dmapp().settings_wnd.reset(new SettingsWnd(*this));
		}
		dmapp().settings_wnd->toFront(true);
	}
	else if (keychar == 'z')
	{
		zoom_and_review();
		return true;
	}
	else
	{
		show_message("ignoring unknown key '" + key.getTextDescription().toStdString() + "'");
	}

	return false;
}


dm::DMContent & dm::DMContent::set_class(const size_t class_idx)
{
	if (selected_mark >= 0 and (size_t)selected_mark < marks.size())
	{
		if (class_idx >= names.size() - 1)
		{
			Log("class idx \"" + std::to_string(class_idx) + "\" is beyond the last index");
			AlertWindow::showMessageBox(AlertWindow::AlertIconType::WarningIcon, "DarkMark", "Class id #" + std::to_string(class_idx) + " is beyond the highest class defined in " + cfg().get_str(cfg_prefix + "names") + ".");
		}
		else
		{
			auto & m = marks[selected_mark];
			m.class_idx = class_idx;
			m.name = names.at(m.class_idx);
			m.description = names.at(m.class_idx);
			need_to_save = true;
		}
	}

	if (class_idx < names.size() - 1)
	{
		most_recent_class_idx = class_idx;
		const auto & opencv_colour = annotation_colours.at(most_recent_class_idx % annotation_colours.size());
		crosshair_colour = Colour(opencv_colour[2], opencv_colour[1], opencv_colour[0]);
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

	if (image_filenames.empty())
	{
		return *this;
	}

	// remember the current image filename so we can scroll back to the same one once we're done sorting
	const std::string old_filename = image_filenames.at(image_filename_index);

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

	size_t idx = 0;
	if (sort_order != ESort::kRandom)
	{
		// as long as the sort order isn't random, then find the previous image within the newly sorted images
		for (idx = 0; idx < image_filenames.size(); idx ++)
		{
			if (old_filename == image_filenames[idx])
			{
				break;
			}
		}
	}
	load_image(idx);

	if (scrollfield_width > 0)
	{
		scrollfield.rebuild_entire_field_on_thread();
	}

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


dm::DMContent & dm::DMContent::toggle_shade_rectangles()
{
	shade_rectangles = not shade_rectangles;
	cfg().setValue("shade_rectangles", shade_rectangles);
	rebuild_image_and_repaint();

	return *this;
}


dm::DMContent & dm::DMContent::toggle_bold_labels()
{
	all_marks_are_bold = not all_marks_are_bold;
	cfg().setValue("all_marks_are_bold", all_marks_are_bold);
	rebuild_image_and_repaint();

	return *this;
}


dm::DMContent & dm::DMContent::toggle_show_predictions(const EToggle toggle)
{
	if (show_predictions != toggle)
	{
		show_predictions = toggle;
		cfg().setValue("show_predictions", static_cast<int>(show_predictions));
	}

	// rebuilding the cache image isn't enough here, we need to completely reload the image so darknet can process the image
	load_image(image_filename_index);

	return *this;
}


dm::DMContent & dm::DMContent::toggle_show_marks()
{
	show_marks = not show_marks;

	cfg().setValue("show_marks", show_marks);

	rebuild_image_and_repaint();

	return *this;
}


dm::DMContent & dm::DMContent::toggle_show_processing_time()
{
	show_processing_time = not show_processing_time;

	cfg().setValue("show_processing_time", show_processing_time);

	rebuild_image_and_repaint();

	return *this;
}


dm::DMContent & dm::DMContent::load_image(const size_t new_idx, const bool full_load)
{
	images_are_loading = true;

	if (need_to_save)
	{
		save_json();
		save_text();
	}

	zoom_review_marks_remaining.clear();
	darknet_image_processing_time = "";
	selected_mark	= -1;
	original_image	= cv::Mat();
	marks.clear();
	image_is_completely_empty = false;

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

	if (dmapp().jump_wnd)
	{
		Slider & slider = dmapp().jump_wnd->slider;
		slider.setValue(image_filename_index + 1);
	}

	bool exception_caught = false;
	std::string what_msg;
	std::string task = "[unknown]";
	try
	{
		task = "loading image file " + long_filename;
//		Log("loading image " + long_filename);
		original_image = cv::imread(image_filenames.at(image_filename_index));

		if (full_load)
		{
			task = "loading json file " + json_filename;
			bool success = load_json();
			if (not success)
			{
				// only attempt to load the .txt file if there was no .json file to process
				task = "importing text file " + text_filename;
				success = load_text();
			}

			if (success and (File(json_filename).existsAsFile() != File(text_filename).existsAsFile()))
			{
				// we either have the .json without the .txt, or the other way around, in which case we need to re-save the files
				need_to_save = true;
			}

			if (show_predictions != EToggle::kOff)
			{
				if (dmapp().darkhelp_nn)
				{
					task = "getting predictions";
					darkhelp_nn().predict(original_image);
					darknet_image_processing_time = darkhelp_nn().duration_string();
					Log("darkhelp processed " + short_filename + " in " + darknet_image_processing_time);

//					std::cout << darkhelp().prediction_results << std::endl;

					// convert the predictions into marks
					task = "converting predictions";
					for (auto prediction : darkhelp_nn().prediction_results)
					{
						Mark m(prediction.original_point, prediction.original_size, original_image.size(), prediction.best_class);
						m.name = names.at(m.class_idx);
						m.description = prediction.name;
						m.is_prediction = true;
						marks.push_back(m);
					}
				}
			}

			// Sort the marks based on a gross (rounded) X and Y position of the midpoint.  This way when
			// the user presses TAB or SHIFT+TAB the marks appear in a consistent and predictable order.
			task = "sorting marks";
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
	}
	catch(const std::exception & e)
	{
		exception_caught = true;
		Log("Error with " + long_filename + ": exception caught while " + task + ": " + e.what());
		what_msg = e.what();
	}
	catch(...)
	{
		exception_caught = true;
		Log("Error: failed while " + task);
		what_msg = "\"unknown\"";
	}

	if (full_load)
	{
		images_are_loading = false;
	}

	if (exception_caught)
	{
		original_image = cv::Mat(32, 32, CV_8UC3, cv::Scalar(0, 0, 255)); // use a red square to indicate a problem
		AlertWindow::showMessageBoxAsync(
			AlertWindow::AlertIconType::WarningIcon,
			"DarkMark",
			"Failure occurred while " + task + ". This happened while processing " + short_filename + ". See the log file for details.\n"
			"\n"
			"A possible cause for this failure is when Darknet has been recently updated, but the version of DarkHelp installed is for an older version of libdarknet. If this is the case, then rebuilding DarkHelp should fix the issue.\n"
			"\n"
			"Another is if the image cannot be opened or loaded correctly by OpenCV.\n"
			"\n"
			"The exact error message logged is: " + what_msg);
	}
	else if (full_load)
	{
		resized();
		rebuild_image_and_repaint();
	}

	return *this;
}


dm::DMContent & dm::DMContent::save_text()
{
	if (text_filename.empty() == false)
	{
		bool delete_txt_file = true;

		if (image_is_completely_empty)
		{
			delete_txt_file = false;
		}

		std::ofstream fs(text_filename);
		for (const auto & m : marks)
		{
			if (m.is_prediction)
			{
				// skip this one since it is a prediction, not a full mark
				continue;
			}

			delete_txt_file = false;

			const cv::Rect2d r	= m.get_normalized_bounding_rect();
			const double w		= r.width;
			const double h		= r.height;
			const double x		= r.x + w / 2.0;
			const double y		= r.y + h / 2.0;
			fs << std::fixed << std::setprecision(10) << m.class_idx << " " << x << " " << y << " " << w << " " << h << std::endl;
		}

		fs.close();

		if (delete_txt_file)
		{
			// there was no legitimate reason to keep the .txt file
			std::remove(text_filename.c_str());
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
		for (auto & m : marks)
		{
			if (m.is_prediction)
			{
				// skip this one since it is a prediction, not a full mark
				continue;
			}

			root["mark"][next_id]["class_idx"	] = m.class_idx;
			root["mark"][next_id]["name"		] = m.name;

			const cv::Rect2d	r1 = m.get_normalized_bounding_rect();
			const cv::Rect		r2 = m.get_bounding_rect(original_image.size());

			root["mark"][next_id]["rect"]["x"]		= r1.x;
			root["mark"][next_id]["rect"]["y"]		= r1.y;
			root["mark"][next_id]["rect"]["w"]		= r1.width;
			root["mark"][next_id]["rect"]["h"]		= r1.height;
			root["mark"][next_id]["rect"]["int_x"]	= r2.x;
			root["mark"][next_id]["rect"]["int_y"]	= r2.y;
			root["mark"][next_id]["rect"]["int_w"]	= r2.width;
			root["mark"][next_id]["rect"]["int_h"]	= r2.height;

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

		if (next_id == 0 and image_is_completely_empty)
		{
			// no marks were written out, so this must be an empty image
			root["completely_empty"] = true;
		}
		else
		{
			root["completely_empty"] = false;
		}

		if (next_id > 0 or image_is_completely_empty)
		{
			std::ofstream fs(json_filename);
			fs << root.dump(1, '\t') << std::endl;
		}
		else
		{
			// image has no markup -- delete the .json file if it existed
			std::remove(json_filename.c_str());
		}

		if (scrollfield_width > 0)
		{
			scrollfield.update_index(image_filename_index);
			scrollfield.need_to_rebuild_cache_image = true;
		}
	}

	need_to_save = false;

	return *this;
}


dm::DMContent & dm::DMContent::import_text_annotations(const VStr & images_fn)
{
	if (images_fn.empty() == false)
	{
		DMContentImportTxt helper(*this, images_fn);
		helper.runThread(); // waits for this to finish before continuing
	}

	return *this;
}


size_t dm::DMContent::count_marks_in_json(File & f, const bool for_sorting_purposes)
{
	size_t result = 0;

	if (f.existsAsFile())
	{
		try
		{
			json root = json::parse(f.loadFileAsString().toStdString());
			result = root["mark"].size();

			if (result > 0 and for_sorting_purposes)
			{
				// add 1 when we're counting for sorting purposes, that way
				// empty images wont be mixed up with images that have 1 mark
				result ++;
			}

			if (result == 0 and root.value("completely_empty", false))
			{
				// if there are zero marks, then see if the image has been identified
				// as completely empty, and if so count that as if it was a mark
				result = 1;
			}
		}
		catch (const std::exception & e)
		{
			Log("Error parsing " + f.getFullPathName().toStdString() + ": " + e.what());
			AlertWindow::showMessageBox(
				AlertWindow::AlertIconType::WarningIcon,
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

			if (class_idx >= static_cast<int>(names.size()))
			{
				Log("ERROR: the annotations in " + text_filename + " references class #" + std::to_string(class_idx) + " but the neural network doesn't have that many classes!?");
				success = false;
				marks.clear();
				break;
			}

			if (class_idx >= 0 and x > 0.0 and y > 0.0 and w > 0.0 and h > 0.0)
			{
				Mark m(cv::Point2d(x, y), cv::Size2d(w, h), cv::Size(0, 0), class_idx);

				m.name = names.at(class_idx);
				m.description = m.name;
				marks.push_back(m);
			}
			else
			{
				Log("ERROR: invalid annotations in " + text_filename +
					": class=" + std::to_string(class_idx) +
					" x=" + std::to_string(x) +
					" y=" + std::to_string(y) +
					" w=" + std::to_string(w) +
					" h=" + std::to_string(h));
				success = false;
				marks.clear();
				break;
			}
		}

		if (success and marks.empty())
		{
			image_is_completely_empty = true;
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

		if (marks.empty())
		{
			image_is_completely_empty = root.value("completely_empty", false);
		}

		success = true;
	}

	return success;
}


dm::DMContent & dm::DMContent::show_darknet_window()
{
	if (not dmapp().darknet_wnd)
	{
		dmapp().darknet_wnd.reset(new DarknetWnd(*this));
	}
	dmapp().darknet_wnd->toFront(true);

	return *this;
}


dm::DMContent & dm::DMContent::delete_current_image()
{
	if (image_filename_index < image_filenames.size())
	{
		need_to_save = false;
		File f(image_filenames[image_filename_index]);
		Log("deleting the file at index #" + std::to_string(image_filename_index) + ": " + f.getFullPathName().toStdString());

		f.moveToTrash();
		f.withFileExtension(".txt"	).moveToTrash();
		f.withFileExtension(".json"	).moveToTrash();

		image_filenames.erase(image_filenames.begin() + image_filename_index);
		load_image(image_filename_index);
		scrollfield.rebuild_entire_field_on_thread();
	}

	return *this;
}


bool dm::DMContent::copy_marks_from_given_image(const std::string & fn)
{
	File f = File(fn).withFileExtension(".json");
	if (f.existsAsFile() == false)
	{
		// keep looking
		return false;
	}

	json root;
	try
	{
		root = json::parse(f.loadFileAsString().toStdString());
	}
	catch (const std::exception & e)
	{
		// ignore the error, nothing we can do about broken .json files
		Log("Error while parsing " + fn + ": " + e.what());
	}

	if (root["mark"].empty())
	{
		// no marks, so keep looking for a better image we can use
		return false;
	}

	// if we get here then we've found an image from which we'll copy marks

	size_t count_added		= 0;
	size_t count_skipped	= 0;

	// re-create the marks from the content of the .json file
	for (auto m : root["mark"])
	{
		Mark new_mark;
		new_mark.class_idx		= m["class_idx"];
		new_mark.name			= m["name"];
		new_mark.description	= new_mark.name;
		new_mark.normalized_all_points.clear();
		for (size_t point_idx = 0; point_idx < m["points"].size(); point_idx ++)
		{
			cv::Point2d p;
			p.x = m["points"][point_idx]["x"];
			p.y = m["points"][point_idx]["y"];
			new_mark.normalized_all_points.push_back(p);
		}
		new_mark.rebalance();

		// check to see if we already have this mark
		bool already_exists = false;
		for (const auto & old_mark : marks)
		{
			if (old_mark.normalized_corner_points == new_mark.normalized_corner_points && old_mark.class_idx == new_mark.class_idx)
			{
				count_skipped ++;
				already_exists = true;
				break;
			}
		}
		if (already_exists == false)
		{
			marks.push_back(new_mark);
			count_added ++;
		}
	}

	std::stringstream ss;
	if (count_added)
	{
		ss << "copied " << count_added << " mark" << (count_added == 1 ? "" : "s");
	}
	if (count_skipped)
	{
		if (count_added)
		{
			ss << " and ";
		}

		ss << "skipped " << count_skipped << " identical mark" << (count_skipped == 1 ? "" : "s");
	}
	ss << " from " << File(fn).getFileName().toStdString();

	show_message(ss.str());

	if (count_added)
	{
		need_to_save = true;
		rebuild_image_and_repaint();
	}

	// we've now copied over all the old marks we need, so stop looking at other images

	return true;
}


dm::DMContent & dm::DMContent::copy_marks_from_next_image()
{
	// first we need to make a copy of the image list and sort it alphabetically;
	// this helps us identify exactly which image is "previous" (assuming images are numbered!)
	auto alphabetical_image_filenames = image_filenames;
	std::sort(alphabetical_image_filenames.begin(), alphabetical_image_filenames.end());

	// find the current index within the alphabetical list
	size_t idx;
	for (idx = 0; idx < alphabetical_image_filenames.size(); idx ++)
	{
		if (alphabetical_image_filenames[idx] == image_filenames[image_filename_index])
		{
			break;
		}
	}

	// now go through all the following images one at a time until we find one that has marks we can copy
	bool done = false;
	while (not done)
	{
		if (idx == alphabetical_image_filenames.size() - 1)
		{
			// there is no more previous images -- nothing we can do
			break;
		}

		idx ++;

		done = copy_marks_from_given_image(alphabetical_image_filenames.at(idx));
	}

	if (not done)
	{
		show_message("no images with marks were found");
	}

	return *this;
}


dm::DMContent & dm::DMContent::copy_marks_from_previous_image()
{
	// first we need to make a copy of the image list and sort it alphabetically;
	// this helps us identify exactly which image is "previous" (assuming images are numbered!)
	auto alphabetical_image_filenames = image_filenames;
	std::sort(alphabetical_image_filenames.begin(), alphabetical_image_filenames.end());

	// find the current index within the alphabetical list
	size_t idx;
	for (idx = 0; idx < alphabetical_image_filenames.size(); idx ++)
	{
		if (alphabetical_image_filenames[idx] == image_filenames[image_filename_index])
		{
			break;
		}
	}

	// now go through all the previous images one at a time until we find one that has marks we can copy
	bool done = false;
	while (not done)
	{
		if (idx == 0)
		{
			// there is no more previous images -- nothing we can do
			break;
		}

		idx --;

		done = copy_marks_from_given_image(alphabetical_image_filenames.at(idx));
	}

	if (not done)
	{
		show_message("no images with marks were found");
	}

	return *this;
}


dm::DMContent & dm::DMContent::accept_current_mark()
{
	if (selected_mark >= 0 and (size_t)selected_mark < marks.size())
	{
		auto & m = marks.at(selected_mark);
		if (m.is_prediction)
		{
			m.is_prediction	= false;
			m.name			= names.at(m.class_idx);
			m.description	= names.at(m.class_idx);
			need_to_save	= true;
			rebuild_image_and_repaint();
		}
	}
	else
	{
		// if no specific mark is selected, then accept all marks
		accept_all_marks();
	}

	return *this;
}


dm::DMContent & dm::DMContent::accept_all_marks()
{
	if (marks.empty() == false)
	{
		// do nothing if we already have 1 or more full marks
		bool ok_to_continue = true;
		for (auto & m : marks)
		{
			if (m.is_prediction == false)
			{
				ok_to_continue = false;
				break;
			}
		}

		if (ok_to_continue)
		{
			for (auto & m : marks)
			{
				m.is_prediction	= false;
				m.name			= names.at(m.class_idx);
				m.description	= names.at(m.class_idx);
			}

			need_to_save = true;
			rebuild_image_and_repaint();
		}
	}

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
	for (size_t idx = 0; idx < names.size() - 1; idx ++)
	{
		const std::string & name = std::to_string(idx) + " - " + names.at(idx);

		const bool is_ticked = (selected_class_idx == (int)idx ? true : false);

		if (idx % 10 == 0 and names.size() - 1 > 1)
		{
			std::stringstream ss;
			if (idx == 10) ss << "CTRL + ";
			if (idx == 20) ss << "ALT + ";
			if (idx == 30) ss << "CTRL + ALT + ";
			ss << "0";
			const size_t max_val = std::min(names.size() - 2, idx + 9) - idx;
			if (max_val > 0)
			{
				ss << " to " << max_val;
			}
			m.addSectionHeader(ss.str());
		}

		m.addItem(name, (is_enabled and not is_ticked), is_ticked, std::function<void()>( [&, idx]{ this->set_class(idx); } ));
	}

	bool image_already_marked = false;
	for (const auto & mark : marks)
	{
		if (mark.is_prediction == false)
		{
			image_already_marked = true;
			image_is_completely_empty = false;
			break;
		}
	}

	m.addSeparator();
	m.addItem("empty image", (image_already_marked == false), image_is_completely_empty, std::function<void()>( [&]
	{
		image_is_completely_empty = ! image_is_completely_empty;
		rebuild_image_and_repaint();
		need_to_save = true;
	} ));

	return m;
}


PopupMenu dm::DMContent::create_popup_menu()
{
	PopupMenu classMenu = create_class_menu();

	PopupMenu labels;
	labels.addItem("always show labels"	, (show_labels != EToggle::kOn	), (show_labels == EToggle::kOn		), std::function<void()>( [&]{ set_labels(EToggle::kOn);	} ));
	labels.addItem("never show labels"	, (show_labels != EToggle::kOff	), (show_labels == EToggle::kOff	), std::function<void()>( [&]{ set_labels(EToggle::kOff);	} ));
	labels.addItem("auto show labels"	, (show_labels != EToggle::kAuto), (show_labels == EToggle::kAuto	), std::function<void()>( [&]{ set_labels(EToggle::kAuto);	} ));
	labels.addSeparator();
	labels.addItem("bold", true, all_marks_are_bold, std::function<void()>( [&]{ toggle_bold_labels(); } ));

	PopupMenu sort;
	sort.addItem("sort alphabetically"				, true, (sort_order == ESort::kAlphabetical	), std::function<void()>( [&]{ set_sort_order(ESort::kAlphabetical	); } ));
	sort.addItem("sort by modification timestamp"	, true, (sort_order == ESort::kTimestamp	), std::function<void()>( [&]{ set_sort_order(ESort::kTimestamp		); } ));
	sort.addItem("sort by number of marks"			, true, (sort_order == ESort::kCountMarks	), std::function<void()>( [&]{ set_sort_order(ESort::kCountMarks	); } ));
	sort.addItem("sort randomly"					, true, (sort_order == ESort::kRandom		), std::function<void()>( [&]{ set_sort_order(ESort::kRandom		); } ));

	PopupMenu view;
	view.addItem("always show darknet predictions"	, (show_predictions != EToggle::kOn		), (show_predictions == EToggle::kOn	), std::function<void()>( [&]{ toggle_show_predictions(EToggle::kOn);	} ));
	view.addItem("never show darknet predictions"	, (show_predictions != EToggle::kOff	), (show_predictions == EToggle::kOff	), std::function<void()>( [&]{ toggle_show_predictions(EToggle::kOff);	} ));
	view.addItem("auto show darknet predictions"	, (show_predictions != EToggle::kAuto	), (show_predictions == EToggle::kAuto	), std::function<void()>( [&]{ toggle_show_predictions(EToggle::kAuto);	} ));
	view.addSeparator();
	view.addItem("show darknet processing time"		, (show_predictions != EToggle::kOff	), (show_processing_time				), std::function<void()>( [&]{ toggle_show_processing_time();			} ));
	view.addSeparator();
	view.addItem("show marks"						, true									, show_marks							,  std::function<void()>( [&]{ toggle_show_marks();						} ));
	view.addItem("shade"							, true									, shade_rectangles						,  std::function<void()>( [&]{ toggle_shade_rectangles();				} ));

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
	image.addItem("accept " + std::to_string(number_of_darknet_marks) + " pending mark" + (number_of_darknet_marks == 1 ? "" : "s"), (number_of_darknet_marks > 0)	, false	, std::function<void()>( [&]{ accept_all_marks();			} ));

	std::string text = "erase 1 mark";
	if (marks.size() != 1)
	{
		text = "erase all " + std::to_string(marks.size()) + " marks";
	}
	image.addItem(text																					, has_any_marks, false	, std::function<void()>( [&]{ erase_all_marks();			} ));
	image.addItem("delete image from disk"																						, std::function<void()>( [&]{ delete_current_image();		} ));
	image.addSeparator();
	image.addItem("jump..."																										, std::function<void()>( [&]{ show_jump_wnd();				} ));
	image.addSeparator();
	image.addItem("move empty images..."																						, std::function<void()>( [&]{ move_empty_images();			} ));
	image.addItem("re-load and re-save every image"																				, std::function<void()>( [&]{ reload_resave_every_image();	} ));
	image.addSeparator();
	image.addItem("flip images..."																								, std::function<void()>( [&]{ flip_images();				} ));
	image.addItem("rotate images..."																							, std::function<void()>( [&]{ rotate_every_image();			} ));
	image.addItem("delete rotate and flip images..."																			, std::function<void()>( [&]{ delete_rotate_and_flip_images(); }));

	PopupMenu help;
	help.addItem("about..."				, std::function<void()>( [&]{ dmapp().about_wnd.reset(new AboutWnd); } ));
	help.addItem("keyboard shortcuts...", std::function<void()>( [&]{ juce::URL("https://www.ccoderun.ca/darkmark/Keyboard.html"				).launchInDefaultBrowser(); } ));
	help.addItem("darknet faq..."		, std::function<void()>( [&]{ juce::URL("https://www.ccoderun.ca/programming/2020-09-25_Darknet_FAQ/"	).launchInDefaultBrowser(); } ));
	help.addItem("discord..."			, std::function<void()>( [&]{ juce::URL("https://discord.gg/zSq8rtW"									).launchInDefaultBrowser(); } ));

	PopupMenu review;
	review.addItem("zoom-and-review"	, std::function<void()>( [&]{ zoom_and_review();	} ));
	review.addItem("review marks..."	, std::function<void()>( [&]{ review_marks();		} ));

	PopupMenu m;
	m.addSubMenu("class", classMenu, classMenu.containsAnyActiveItems());
	m.addSubMenu("labels", labels);
	m.addSubMenu("sort", sort);
	m.addSubMenu("view", view);
	m.addSubMenu("image", image);
	m.addSubMenu("review", review);
	m.addSubMenu("help", help);

	m.addSeparator();
	m.addItem("gather statistics..."	, std::function<void()>( [&]{ gather_statistics();		} ));
	m.addItem("create darknet files..."	, std::function<void()>( [&]{ show_darknet_window();	} ));
	m.addItem("other settings..."		, std::function<void()>( [&]
	{
		if (not dmapp().settings_wnd)
		{
			dmapp().settings_wnd.reset(new SettingsWnd(*this));
		}
		dmapp().settings_wnd->toFront(true);
	}));

	return m;
}


dm::DMContent & dm::DMContent::gather_statistics()
{
	if (need_to_save)
	{
		save_json();
		save_text();
	}

	DMContentStatistics helper(*this);
	helper.runThread();

	return *this;
}


dm::DMContent & dm::DMContent::review_marks()
{
	if (need_to_save)
	{
		save_json();
		save_text();
	}

	DMContentReview helper(*this);
	helper.runThread();

	return *this;
}


dm::DMContent & dm::DMContent::zoom_and_review()
{
	bool use_current_image = false;

	if (user_specified_zoom_factor <= 0.0)
	{
		// we're turning on the "zoom review" feature
		keyPressed(KeyPress::createFromDescription("spacebar"));
		Thread::yield();
		use_current_image = true;
		zoom_review_marks_remaining.clear();
		Log("starting zoom review");
	}
	else
	{
		// see if we can eliminate any of the existing marks and see what remains
		std::set<size_t> indexes_to_remove;

		const cv::Rect r = cv::Rect(
			canvas.zoom_image_offset.x,
			canvas.zoom_image_offset.y,
			canvas.getWidth(),
			canvas.getHeight());

		for (const auto i : zoom_review_marks_remaining)
		{
			if (i >= marks.size())
			{
				// mark must have been removed!?
				indexes_to_remove.insert(i);
				continue;
			}

			auto & mark = marks.at(i);
			const auto bounding_rect = mark.get_bounding_rect(scaled_image_size);
			if ((r & bounding_rect) == bounding_rect)
			{
				indexes_to_remove.insert(i);
			}
		}
		for (const auto i : indexes_to_remove)
		{
			zoom_review_marks_remaining.erase(i);
		}
		for (const auto i : zoom_review_marks_remaining)
		{
			Log("zoom review: mark remaining to be shown in this image: " + marks.at(i).name);
		}
	}

	while (zoom_review_marks_remaining.empty())
	{
		// something needs to happen because we don't have anything to show!

		bool image_found = false;
		if (use_current_image == false)
		{
			// we know for certain we need to find a new image to display
			Log("zoom review: looking for a new image to use, current image index is " + std::to_string(image_filename_index));
			for (size_t idx = image_filename_index + 1; idx < image_filenames.size(); idx ++)
			{
				File f = File(image_filenames[idx]).withFileExtension(".txt");
				if (f.existsAsFile() and f.getSize() > 10)
				{
					load_image(idx);
					image_found = true;
					break;
				}
			}
		}

		// remember all of the marks that we need to display to the user
		if (image_is_completely_empty == false)
		{
			/* Cheat!  We want to review the marks from largest down to smallest.  This way, if the largest marks include a bunch
			 * of embedded markes, by viewing the large ones first we can remove the small ones from the list to be reviewed.
			 * For example:  license plates, where we have a large "plate" mark around the other annotations.
			 *
			 * So to make this happen easily, we'll temporarily modify the sort order of the marks, and order them from largest
			 * down to smallest.
			 */
			std::sort(marks.begin(), marks.end(),
					[](auto & lhs, auto & rhs)
					{
						const auto lhs_area = lhs.get_normalized_bounding_rect().area();
						const auto rhs_area = rhs.get_normalized_bounding_rect().area();
						return lhs_area > rhs_area;
					} );

			for (size_t idx = 0; idx < marks.size(); idx ++)
			{
				const auto & m = marks.at(idx);
				if (m.is_prediction)
				{
					continue;
				}

				Log("zoom review: from image #" + std::to_string(image_filename_index) + ": remembering mark #" + std::to_string(idx) + ": " + m.name);
				zoom_review_marks_remaining.insert(idx);
			}

			if (use_current_image == false and image_found == false)
			{
				// we've reached the end of the list of images, no use in trying to loop more
				break;
			}
		}

		use_current_image = false;
	}

	if (zoom_review_marks_remaining.empty() == false)
	{
		Log("zoom_review_marks_remaining contains " + std::to_string(zoom_review_marks_remaining.size()) + " items");

		size_t idx = *zoom_review_marks_remaining.begin();
		zoom_review_marks_remaining.erase(idx);

		selected_mark = idx;
		auto & m = marks.at(idx);
		const cv::Rect r = m.get_bounding_rect(scaled_image_size);

		Log("extracted mark idx #" + std::to_string(idx) + " from zoom_review_marks_remaining: " + m.name);

		zoom_point_of_interest.x = (r.x + r.width	/ 2) / current_zoom_factor;
		zoom_point_of_interest.y = (r.y + r.height	/ 2) / current_zoom_factor;

		Log("showing idx #" + std::to_string(idx) + " (" + m.name + ") at x=" + std::to_string(zoom_point_of_interest.x) + " y=" + std::to_string(zoom_point_of_interest.y));

		rebuild_image_and_repaint();
	}

	return *this;
}


dm::DMContent & dm::DMContent::rotate_every_image()
{
	DMContentRotateImages rotateImages(*this);
	rotateImages.runModalLoop();

	return *this;
}


dm::DMContent & dm::DMContent::flip_images()
{
	DMContentFlipImages flipImages(*this);
	flipImages.runModalLoop();

	return *this;
}


dm::DMContent & dm::DMContent::delete_rotate_and_flip_images()
{
	const int result = AlertWindow::showOkCancelBox(AlertWindow::QuestionIcon, "DarkMark",
		"Filename of images rotated by DarkMark end in _r090, _r180, or _r270.\n"
		"Filename of images flipped by DarkMark end in _fh or _fv.\n"
		"\n"
		"If you continue, this will delete these rotated and flipped images.  Additionally, "
		"this will also delete the corresponding annotations such as .txt and .json files.\n"
		"\n"
		"Delete all rotated/flipped images and annotations?", "Delete Files");

	if (result != 0)
	{
		DMContentDeleteRotateAndFlipImages helper(*this);
		helper.runThread();
	}

	return *this;
}


dm::DMContent & dm::DMContent::move_empty_images()
{
	const int result = AlertWindow::showOkCancelBox(AlertWindow::QuestionIcon, "DarkMark",
		"Some people like to organize their images so \"empty\" (aka \"negative sample\") images are stored together. This has "
		"zero impact on how the neural network is trained. The length of time to train won't change, and the effectiveness of "
		"the neural network will be exactly the same. The only real purpose is to help people organize their images for review.\n"
		"\n"
		"Do you wish to move the empty images (aka \"negative samples\") into a folder called \"empty_images\"?");

	if (result != 0)
	{
		DMContentMoveEmptyImages helper(*this);
		helper.runThread();
	}

	return *this;
}


dm::DMContent & dm::DMContent::reload_resave_every_image()
{
	DMContentReloadResave helper(*this);
	helper.runThread();

	return *this;
}


dm::DMContent & dm::DMContent::show_jump_wnd()
{
	if (not dmapp().jump_wnd)
	{
		dmapp().jump_wnd.reset(new DMJumpWnd(*this));
	}
	dmapp().jump_wnd->toFront(true);

	return *this;
}


dm::DMContent & dm::DMContent::show_message(const std::string & msg)
{
	if (msg.empty())
	{
		bubble_message.setVisible(false);
	}
	else
	{
		AttributedString str;
		str.setText(msg);
		str.setColour(Colours::white);

		const Rectangle<int> r(getWidth()/2, 1, 1, 1);
		bubble_message.showAt(r, str, 4000, true, false);
		Log("bubble message: " + msg);
	}

	return *this;
}


dm::DMContent & dm::DMContent::save_screenshot(const bool full_size, const std::string & fn)
{
	bool proceed = false;
	std::string filename = fn;
	if (filename.empty())
	{
		filename = File(long_filename).getFileNameWithoutExtension().toStdString();
		filename += "_annotated.png";
	}
	else
	{
		proceed = true;
	}

	File f = File::getSpecialLocation(File::SpecialLocationType::userDesktopDirectory).getChildFile(filename);
	if (fn.empty())
	{
		FileChooser chooser("Save annotated image to...", f, "*.png,*.jpg,*.jpeg");
		if (chooser.browseForFileToSave(true))
		{
			f = chooser.getResult();
			proceed = true;
		}
	}

	if (proceed)
	{
		const auto old_scaled_image_size = scaled_image_size;

		if (full_size) // uppercase 'S' means we should use the full-size image
		{
			// we want to save the full-size image, not the resized one we're currently viewing,
			// so swap out a few things, re-build the annotated image, and save *those* results
			scaled_image_size = original_image.size();
			canvas.rebuild_cache_image();
		}

		if (f.hasFileExtension(".png"))
		{
			cv::imwrite(f.getFullPathName().toStdString(), scaled_image, {CV_IMWRITE_PNG_COMPRESSION, 9});
		}
		else
		{
			cv::imwrite(f.getFullPathName().toStdString(), scaled_image, {CV_IMWRITE_JPEG_QUALITY, 75});
		}

		if (scaled_image_size != old_scaled_image_size)
		{
			// now put back the scaled image we expect to be there
			scaled_image_size = old_scaled_image_size;
			canvas.rebuild_cache_image();
		}
	}

	return *this;
}


dm::DMContent & dm::DMContent::highlight_rectangle(const cv::Rect & r)
{
	stopTimer();

	if (r.area() > 0)
	{
		// this is used in paintOverChildren() to draw a circle on top of the image

		highlight_x				= scale_factor * (r.x + r.width/2);
		highlight_y				= scale_factor * (r.y + r.height/2);
		highlight_inside_size	= 250.0f;
		highlight_outside_size	= 250.0f;

		startTimer(50); // milliseconds
	}

	return *this;
}


void dm::DMContent::timerCallback()
{
	// used by highlight_rectangle() and paintOverChildren()

	repaint();
}
