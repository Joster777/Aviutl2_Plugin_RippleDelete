//----------------------------------------------------------------------------------
//	リップル削除 汎用プラグイン (.aux2) for AviUtl ExEdit2
//
//	選択中(点線で囲われている)のオブジェクトの、先頭から再生位置の手前までを取り除き、
//	そのオブジェクトと同じレイヤーの後ろのオブジェクトを、取り除いた長さ分だけ左に詰めます。
//	オブジェクトの削除・作成は行わず、元のオブジェクトの開始位置を縮めるので、
//	グループ化されている場合もグループ化が維持されます。
//
//	・対象は「選択中のオブジェクト」だけです。選択されていない場合は何もしません。
//	  (編集メニュー・ショートカットから呼ばれて選択中のオブジェクトを取得できない場合は、
//	   オブジェクト設定ウィンドウで選択中のオブジェクトを対象にします)
//	・対象のレイヤーは、選択中のオブジェクト自身のレイヤーから取得します。
//	  (選択中のレイヤー、カーソルのあるレイヤーは参照しません)
//	・再生位置が選択中のオブジェクトの範囲外の場合は、そのオブジェクトには何もしません。
//	・リップル削除のあとは、残ったオブジェクトを選択状態にします。
//	・リップル削除のあとは、再生バーを残ったオブジェクトの開始位置(カットしたフレームと同じ映像が表示される位置)に移動します。
//	・リップル削除の前後で、フレーム範囲選択(ルーラー上の水色の範囲)は変更しません。
//----------------------------------------------------------------------------------
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "plugin2.h"

// UTF-8の文字列リテラル (C++17/C++20どちらでもconst char*として扱う)
#define U8(s) reinterpret_cast<const char*>(u8##s)

//---------------------------------------------------------------------
//	汎用プラグイン構造体定義
//---------------------------------------------------------------------
COMMON_PLUGIN_TABLE common_plugin_table = {
	L"Ripple Delete",
	L"Ripple Delete version 1.12",
};

EXTERN_C __declspec(dllexport) DWORD RequiredVersion()
{
	return 2003300;
}

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void)
{
	return &common_plugin_table;
}

//---------------------------------------------------------------------
//	文字列ユーティリティ
//---------------------------------------------------------------------
static std::wstring to_wide(const std::string& text)
{
	if (text.empty())
	{
		return std::wstring();
	}
	int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
	std::wstring result(length, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &result[0], length);
	return result;
}

static std::vector<std::string> split_text(const std::string& text, char separator)
{
	std::vector<std::string> tokens;
	size_t pos = 0;
	while (pos <= text.size())
	{
		size_t end = text.find(separator, pos);
		if (end == std::string::npos)
		{
			end = text.size();
		}
		tokens.push_back(text.substr(pos, end - pos));
		pos = end + 1;
	}
	return tokens;
}

static std::string join_text(const std::vector<std::string>& tokens, char separator)
{
	std::string result;
	for (size_t i = 0; i < tokens.size(); i++)
	{
		if (i > 0)
		{
			result += separator;
		}
		result += tokens[i];
	}
	return result;
}

static std::vector<std::string> split_lines(const std::string& text)
{
	std::vector<std::string> lines = split_text(text, '\n');
	for (std::string& line : lines)
	{
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
	}
	return lines;
}

static bool starts_with(const std::string& text, const std::string& prefix)
{
	return text.compare(0, prefix.size(), prefix) == 0;
}

static std::string format_number(double value)
{
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "%.3f", value);
	return buffer;
}

//---------------------------------------------------------------------
//	再生位置の項目情報
//	動画ファイルの再生位置は「6.000,30.451,再生範囲,0」のように
//	「開始値,終了値,モード,追加値」の形式で保存されている
//---------------------------------------------------------------------
struct POSITION_ITEM
{
	std::string effect;         // エフェクト名 (同名が複数ある場合は ":n" 付き)
	std::string item;           // 設定項目名 (再生位置)
	std::string left_value;     // 左側に設定する値 (変更が不要なら空)
	std::string right_value;    // 右側に設定する値
	double right_first = 0.0;   // 右側の1つ目の数値
	double right_second = 0.0;  // 右側の2つ目の数値
	bool has_second = false;    // 2つ目の数値があるか
};

//---------------------------------------------------------------------
//	右側オブジェクト用のエイリアスデータを作る
//	・フレーム情報を「frame=カット位置,元の終了フレーム」に書き換える
//	・再生位置を書き換える
//	    再生範囲モード(開始値,終了値) : 開始値をカット位置の値に進める (終了値はそのまま)
//	                                    左側は終了値をカット位置の値にする
//	    単独の値                      : カットした秒数 × 再生速度 だけ進める
//	ratio          : オブジェクト全体の長さに対するカット位置の割合 (0～1)
//	offset_seconds : オブジェクトの先頭からカット位置までの秒数
//---------------------------------------------------------------------
static std::string make_right_alias(const std::string& alias, int cursor, int end, double ratio, double offset_seconds, std::vector<POSITION_ITEM>& items)
{
	std::vector<std::string> lines = split_lines(alias);
	const std::string key_effect = "effect.name=";
	const std::string key_position = U8("再生位置");
	const std::string key_speed = U8("再生速度=");
	const std::string mode_range = U8("再生範囲");

	// 1周目 : セクションごとのエフェクト名と再生速度を調べる
	struct SECTION_INFO
	{
		std::string name;
		double speed = 100.0;
		int index = 0;      // 同名エフェクトの何番目か
	};
	std::vector<SECTION_INFO> sections;
	for (const std::string& line : lines)
	{
		if (!line.empty() && line[0] == '[')
		{
			sections.push_back(SECTION_INFO());
		}
		else if (!sections.empty())
		{
			if (starts_with(line, key_effect))
			{
				sections.back().name = line.substr(key_effect.size());
			}
			else if (starts_with(line, key_speed))
			{
				sections.back().speed = atof(line.c_str() + key_speed.size());
			}
		}
	}
	std::map<std::string, int> name_count;
	for (SECTION_INFO& section : sections)
	{
		if (!section.name.empty())
		{
			section.index = name_count[section.name]++;
		}
	}

	// 2周目 : 行を書き換える
	std::string result;
	int section_index = -1;
	for (const std::string& line : lines)
	{
		if (!line.empty() && line[0] == '[')
		{
			section_index++;
		}
		if (starts_with(line, "frame="))
		{
			result += "frame=" + std::to_string(cursor) + "," + std::to_string(end) + "\n";
			continue;
		}
		size_t equal = line.find('=');
		if (section_index >= 0 && equal != std::string::npos && starts_with(line, key_position))
		{
			std::string key = line.substr(0, equal);
			std::vector<std::string> tokens = split_text(line.substr(equal + 1), ',');
			const SECTION_INFO& section = sections[section_index];

			POSITION_ITEM item;
			item.effect = section.name;
			if (name_count[section.name] > 1)
			{
				item.effect += ":" + std::to_string(section.index);
			}
			item.item = key;

			bool adjusted = false;
			if (tokens.size() == 1)
			{
				// 単独の値
				double position = atof(tokens[0].c_str()) + offset_seconds * section.speed / 100.0;
				item.right_value = format_number(position);
				item.right_first = position;
				adjusted = true;
			}
			else if (tokens.size() >= 3 && tokens[2] == mode_range)
			{
				// 再生範囲モード : 開始値,終了値,再生範囲,追加値
				double first = atof(tokens[0].c_str());
				double second = atof(tokens[1].c_str());
				double cut_value = first + (second - first) * ratio;

				std::vector<std::string> right_tokens = tokens;
				right_tokens[0] = format_number(cut_value);
				item.right_value = join_text(right_tokens, ',');
				item.right_first = cut_value;
				item.right_second = second;
				item.has_second = true;

				std::vector<std::string> left_tokens = tokens;
				left_tokens[1] = format_number(cut_value);
				item.left_value = join_text(left_tokens, ',');
				adjusted = true;
			}

			if (adjusted)
			{
				result += key + "=" + item.right_value + "\n";
				items.push_back(item);
				continue;
			}
		}
		result += line + "\n";
	}
	return result;
}

//---------------------------------------------------------------------
//	オブジェクトの開始位置を cursor まで縮める (先頭から cursor の手前までを取り除く)
//	・新しいオブジェクトを作らず、元のオブジェクトをそのまま使うので、グループ化も維持される
//	・再生位置は、カット位置の値に設定し直す
//	失敗した場合は元の状態に戻して false を返す
//---------------------------------------------------------------------
static bool trim_object_start(EDIT_SECTION* edit, OBJECT_HANDLE object, int cursor)
{
	OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(object);
	if (cursor <= lf.start || cursor > lf.end)
	{
		return false;
	}

	LPCSTR text = edit->get_object_alias(object);
	if (!text)
	{
		return false;
	}
	std::string alias = text; // 次の文字列取得までしか有効でないのでコピーしておく

	// 再生位置などの、カット位置に合わせて書き換える値を計算しておく
	int length = lf.end - lf.start + 1;
	double ratio = (double)(cursor - lf.start) / length;
	double offset = (double)(cursor - lf.start) * edit->info->scale / edit->info->rate;
	std::vector<POSITION_ITEM> items;
	make_right_alias(alias, cursor, lf.end, ratio, offset, items);

	// 開始点を cursor に移動する
	if (!edit->move_object_section(object, 0, cursor))
	{
		return false;
	}

	// 想定どおりの位置・範囲になっていることを確認する (違う場合は元に戻す)
	OBJECT_LAYER_FRAME after = edit->get_object_layer_frame(object);
	if (after.layer != lf.layer || after.start != cursor || after.end != lf.end)
	{
		edit->move_object_section(object, 0, lf.start);
		return false;
	}

	// 再生位置を、カット位置の値に設定する
	for (const POSITION_ITEM& item : items)
	{
		std::wstring effect = to_wide(item.effect);
		std::wstring name = to_wide(item.item);
		edit->set_object_item_value(object, effect.c_str(), name.c_str(), item.right_value.c_str());
	}
	return true;
}

//---------------------------------------------------------------------
//	1つのレイヤーで from_frame 以降のオブジェクトを length フレーム前に詰める
//	ハンドルを溜め込まず、1つ動かすたびに次を探し直す (元の座標系で次を探す)
//---------------------------------------------------------------------
static void shift_layer(EDIT_SECTION* edit, int layer, int from_frame, int length)
{
	int frame = from_frame;
	for (;;)
	{
		OBJECT_HANDLE object = edit->find_object(layer, frame);
		if (!object)
		{
			break;
		}
		OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(object);
		if (lf.end < frame)
		{
			break; // 想定外の結果で無限ループにならないようにする
		}
		if (lf.start >= from_frame)
		{
			edit->move_object(object, lf.layer, lf.start - length);
		}
		frame = lf.end + 1;
	}
}

//---------------------------------------------------------------------
//	選択中のオブジェクト1つに対するリップル削除
//	target : 選択中のオブジェクトのレイヤー・フレーム情報
//	1. オブジェクトの先頭から cursor の手前までを取り除く (開始位置を cursor まで縮める)
//	2. そのオブジェクトと、同じレイヤーの後ろのオブジェクトを、取り除いた長さ分だけ左に詰める
//	3. 再生バーを、オブジェクトの元の開始位置に移動する (カットしたフレームと同じ映像が表示される)
//	オブジェクトの削除・作成は行わない (グループ化が維持される)
//	cursor_layer : 再生バーを移動するときに指定するレイヤー番号 (現在の選択レイヤーのまま変えない。対象の判定には使わない)
//	戻り値 : リップル削除を行った場合はtrue (オブジェクトは target.start の位置に移動している)
//---------------------------------------------------------------------
static bool ripple_delete_object(EDIT_SECTION* edit, const OBJECT_LAYER_FRAME& target, int cursor, int cursor_layer)
{
	// 位置情報から対象オブジェクトを探し直す
	OBJECT_HANDLE object = edit->find_object(target.layer, target.start);
	if (!object)
	{
		return false;
	}
	OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(object);
	if (lf.layer != target.layer || lf.start != target.start)
	{
		return false;
	}

	// 再生位置が対象オブジェクトの範囲外なら何もしない
	if (cursor <= lf.start || cursor > lf.end)
	{
		return false;
	}

	// 1. 先頭から cursor の手前までを取り除く
	if (!trim_object_start(edit, object, cursor))
	{
		return false;
	}

	// 再生バーを、詰めたあとのオブジェクトの開始位置(= 元の開始位置)に移動する
	// ※詰める前に移動しておくことで、最後のオブジェクトで全体の長さが縮んでも、再生バーが範囲外にならない
	edit->set_cursor_layer_frame(cursor_layer, lf.start);
	// edit->set_select_range(-1, -1);

	// 2. 取り除いた長さ分だけ、同じレイヤーの cursor 以降(縮めたオブジェクト自身を含む)を左に詰める
	int length = cursor - lf.start;
	shift_layer(edit, lf.layer, cursor, length);
	return true;
}

//---------------------------------------------------------------------
//	メニュー : リップル削除
//	対象はオブジェクト単位で取得する (選択中のレイヤー・カーソルのレイヤーは参照しない)
//	  1. 選択中(点線)のオブジェクト
//	  2. 1が取得できない場合(編集メニュー・ショートカットからの呼び出し)は
//	     オブジェクト設定ウィンドウで選択中のオブジェクト
//	どちらも無い場合は何もしない
//	リップル削除のあとは、残ったオブジェクトを選択状態にする
//---------------------------------------------------------------------
static void menu_ripple_delete(EDIT_SECTION* edit)
{
	int cursor = edit->info->frame;
	int cursor_layer = edit->info->layer; // 再生バーを移動するときに、選択レイヤーを変えないためだけに使う

	// 処理前のフレーム範囲選択(ルーラー上の水色の範囲)を控えておく ※リップル削除で変わらないように最後に元に戻す
	int range_start = edit->info->select_range_start;
	int range_end = edit->info->select_range_end;

	// 選択中のオブジェクトの位置情報だけを控えておく (ハンドルは使い回さない)
	std::vector<OBJECT_LAYER_FRAME> targets;
	int selected_num = edit->get_selected_object_num();
	for (int i = 0; i < selected_num; i++)
	{
		OBJECT_HANDLE object = edit->get_selected_object(i);
		if (object)
		{
			targets.push_back(edit->get_object_layer_frame(object));
		}
	}
	if (targets.empty())
	{
		OBJECT_HANDLE focus = edit->get_focus_object();
		if (focus)
		{
			targets.push_back(edit->get_object_layer_frame(focus));
		}
	}

	// リップル削除を行い、最初に処理できたオブジェクトを選択し直す対象として控える
	bool has_reselect = false;
	OBJECT_LAYER_FRAME reselect = {};
	for (const OBJECT_LAYER_FRAME& target : targets)
	{
		if (ripple_delete_object(edit, target, cursor, cursor_layer) && !has_reselect)
		{
			reselect = target; // オブジェクトは元の開始位置に移動している
			has_reselect = true;
		}
	}

	// 残ったオブジェクトを選択状態にする (探し直して位置を確認してから設定する)
	if (has_reselect)
	{
		OBJECT_HANDLE right = edit->find_object(reselect.layer, reselect.start);
		if (right)
		{
			OBJECT_LAYER_FRAME right_lf = edit->get_object_layer_frame(right);
			if (right_lf.layer == reselect.layer && right_lf.start == reselect.start)
			{
				edit->set_focus_object(right);
			}
		}
	}

	// フレーム範囲選択を処理前の状態に戻す (未選択の場合は -1,-1 が設定され、選択なしに戻る)
	if (has_reselect)
	{
		edit->set_select_range(range_start, range_end);
		//edit->set_select_range(-1, -1);
	}
}

//---------------------------------------------------------------------
//	プラグイン登録関数
//---------------------------------------------------------------------
EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host)
{
	host->register_edit_menu(L"リップル削除", menu_ripple_delete);
	host->register_object_menu(L"リップル削除", menu_ripple_delete);
}
