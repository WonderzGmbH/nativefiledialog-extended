/*
  Native File Dialog Extended
  Repository: https://github.com/btzy/nativefiledialog-extended
  License: Zlib
  Authors: Bernard Teo, Michael Labbe

  Note: We do not check for malloc failure on Linux - Linux overcommits memory!
*/

#include <assert.h>
#include <dbus/dbus.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>  // for the random token string

#include "nfd.h"

namespace {

template <typename T = void>
T* NFDi_Malloc(size_t bytes) {
    void* ptr = malloc(bytes);
    assert(ptr);  // Linux malloc never fails

    return static_cast<T*>(ptr);
}

template <typename T>
void NFDi_Free(T* ptr) {
    assert(ptr);
    free(static_cast<void*>(ptr));
}

template <typename T>
struct Free_Guard {
    T* data;
    Free_Guard(T* freeable) noexcept : data(freeable) {}
    ~Free_Guard() { NFDi_Free(data); }
};

template <typename T>
struct FreeCheck_Guard {
    T* data;
    FreeCheck_Guard(T* freeable = nullptr) noexcept : data(freeable) {}
    ~FreeCheck_Guard() {
        if (data) NFDi_Free(data);
    }
};

struct DBusMessage_Guard {
    DBusMessage* data;
    DBusMessage_Guard(DBusMessage* freeable) noexcept : data(freeable) {}
    ~DBusMessage_Guard() { dbus_message_unref(data); }
};

/* D-Bus connection handle */
DBusConnection* dbus_conn;
/* current D-Bus error */
DBusError dbus_err;
/* current error (may be a pointer to the D-Bus error message above, or a pointer to some string
 * literal) */
const char* err_ptr = nullptr;
/* the unique name of our connection, used for the Request handle; owned by D-Bus so we don't free
 * it */
const char* dbus_unique_name;

void NFDi_SetError(const char* msg) {
    err_ptr = msg;
}

template <typename T>
T* copy(const T* begin, const T* end, T* out) {
    for (; begin != end; ++begin) {
        *out++ = *begin;
    }
    return out;
}

template <typename T, typename Callback>
T* transform(const T* begin, const T* end, T* out, Callback callback) {
    for (; begin != end; ++begin) {
        *out++ = callback(*begin);
    }
    return out;
}

constexpr const char* STR_EMPTY = "";
constexpr const char* STR_OPEN_FILE = "Open File";
constexpr const char* STR_HANDLE_TOKEN = "handle_token";
constexpr const char* STR_MULTIPLE = "multiple";
constexpr const char* STR_FILTERS = "filters";

void AppendOpenFileQueryDictEntryHandleToken(DBusMessageIter& sub_iter, const char* handle_token) {
    DBusMessageIter sub_sub_iter;
    DBusMessageIter variant_iter;
    dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
    dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_HANDLE_TOKEN);
    dbus_message_iter_open_container(&sub_sub_iter, DBUS_TYPE_VARIANT, "s", &variant_iter);
    dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_STRING, &handle_token);
    dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
    dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);
}

template <bool Multiple>
void AppendOpenFileQueryDictEntryMultiple(DBusMessageIter&);
template <>
void AppendOpenFileQueryDictEntryMultiple<true>(DBusMessageIter& sub_iter) {
    DBusMessageIter sub_sub_iter;
    DBusMessageIter variant_iter;
    dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
    dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_MULTIPLE);
    dbus_message_iter_open_container(&sub_sub_iter, DBUS_TYPE_VARIANT, "b", &variant_iter);
    {
        bool b = true;
        dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_BOOLEAN, &b);
    }
    dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
    dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);
}
template <>
void AppendOpenFileQueryDictEntryMultiple<false>(DBusMessageIter&) {}

void AppendOpenFileQueryDictEntryFilters(DBusMessageIter& sub_iter,
                            const nfdnfilteritem_t* filterList,
                            nfdfiltersize_t filterCount) {
    DBusMessageIter sub_sub_iter;
    DBusMessageIter variant_iter;
    DBusMessageIter filter_list_iter;
    DBusMessageIter filter_list_tuple_iter;
    DBusMessageIter filter_sublist_iter;
    DBusMessageIter filter_sublist_tuple_iter;
    dbus_message_iter_open_container(&sub_iter, DBUS_TYPE_DICT_ENTRY, nullptr, &sub_sub_iter);
    dbus_message_iter_append_basic(&sub_sub_iter, DBUS_TYPE_STRING, &STR_FILTERS);
    dbus_message_iter_open_container(&sub_sub_iter, DBUS_TYPE_VARIANT, "a(sa(us))", &variant_iter);
    dbus_message_iter_open_container(&variant_iter, DBUS_TYPE_ARRAY, "(sa(us))", &filter_list_iter);
    for (nfdfiltersize_t i = 0; i != filterCount; ++i){
        dbus_message_iter_open_container(&filter_list_iter, DBUS_TYPE_TUPLE, "sa(us)", &filter_list_tuple_iter);
        dbus_message_iter_append_basic(&filter_list_tuple_iter, DBUS_TYPE_STRING, &filterList[i].name);
        dbus_message_iter_open_container(&filter_list_tuple_iter, DBUS_TYPE_ARRAY, "(us)", &filter_sublist_iter);
        const char* extn_begin = filterList[i].spec;
        const char* extn_end = extn_begin;
        while (true) {
            dbus_message_iter_open_container(&filter_sublist_iter, DBUS_TYPE_TUPLE, "us", &filter_sublist_tuple_iter);
            {
                const unsigned zero = 0;
                dbus_message_iter_append_basic(&filter_list_tuple_iter, DBUS_TYPE_UNSIGNED, &zero);
            }
            do {
                ++extn_end;
            } while (*extn_end != ',' && *extn_end != '\0');
            if (*extn_end == '\0'){
                dbus_message_iter_append_basic(&filter_list_tuple_iter, DBUS_TYPE_STRING, &extn_begin);
                break;
            }
            else {
                // need to put '\0' at the end
                char* buf = alloca(extn_end - extn_begin + 1);
                copy(extn_begin, extn_end, buf);
                dbus_message_iter_append_basic(&filter_list_tuple_iter, DBUS_TYPE_STRING, &buf);
                extn_begin = extn_end + 1;
                extn_end = extn_begin;
            }
            dbus_message_iter_close_container(&filter_sublist_iter, &filter_sublist_tuple_iter);
        }
        //dbus_message_iter_append_basic(&variant_iter, DBUS_TYPE_STRING, &handle_token);
        dbus_message_iter_close_container(&filter_list_tuple_iter, &filter_sublist_iter);
        dbus_message_iter_close_container(&filter_list_iter, &filter_list_tuple_iter);
    }
    dbus_message_iter_close_container(&variant_iter, &filter_list_iter);
    dbus_message_iter_close_container(&sub_sub_iter, &variant_iter);
    dbus_message_iter_close_container(&sub_iter, &sub_sub_iter);
}

// Append OpenFile() portal params to the given query.
template <bool Multiple>
void AppendOpenFileQueryParams(DBusMessage* query, const char* handle_token,
                            const nfdnfilteritem_t* filterList,
                            nfdfiltersize_t filterCount) {
    DBusMessageIter iter;
    dbus_message_iter_init_append(query, &iter);

    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &STR_EMPTY);

    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &STR_OPEN_FILE);

    DBusMessageIter sub_iter;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &sub_iter);
    AppendOpenFileQueryDictEntryHandleToken(sub_iter, handle_token);
    AppendOpenFileQueryDictEntryMultiple<Multiple>(sub_iter);
    if (filterCount !=0) {
        AppendOpenFileQueryDictEntryFilters(sub_iter, filterList, filterCount);
    }
    dbus_message_iter_close_container(&iter, &sub_iter);
}

nfdresult_t ReadDictImpl(const char*, DBusMessageIter&) {
    return NFD_OKAY;
}

template <typename Callback, typename... Args>
nfdresult_t ReadDictImpl(const char* key,
                         DBusMessageIter& iter,
                         const char*& candidate_key,
                         Callback& candidate_callback,
                         Args&... args) {
    if (strcmp(key, candidate_key) == 0) {
        // this is the correct callback
        return candidate_callback(iter);
    } else {
        return ReadDictImpl(key, iter, args...);
    }
}

// Read a dictionary from the given iterator.  The type of the element under this iterator will be
// checked. The args are alternately key and callback. Key is a const char*, and callback is a
// function that returns nfdresult_t.  Return NFD_ERROR to stop processing and return immediately.
template <typename... Args>
nfdresult_t ReadDict(DBusMessageIter iter, Args... args) {
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        NFDi_SetError("D-Bus response signal argument is not an array.");
        return NFD_ERROR;
    }
    DBusMessageIter sub_iter;
    dbus_message_iter_recurse(&iter, &sub_iter);
    while (dbus_message_iter_get_arg_type(&sub_iter) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter de_iter;
        dbus_message_iter_recurse(&sub_iter, &de_iter);
        if (dbus_message_iter_get_arg_type(&de_iter) != DBUS_TYPE_STRING) {
            NFDi_SetError("D-Bus response signal dict entry does not start with a string.");
            return NFD_ERROR;
        }
        const char* key;
        dbus_message_iter_get_basic(&de_iter, &key);
        if (!dbus_message_iter_next(&de_iter)) {
            NFDi_SetError("D-Bus response signal dict entry is missing one or more arguments.");
            return NFD_ERROR;
        }
        // unwrap the variant
        if (dbus_message_iter_get_arg_type(&de_iter) != DBUS_TYPE_VARIANT) {
            NFDi_SetError("D-Bus response signal dict entry value is not a variant.");
            return NFD_ERROR;
        }
        DBusMessageIter de_variant_iter;
        dbus_message_iter_recurse(&de_iter, &de_variant_iter);
        if (ReadDictImpl(key, de_variant_iter, args...) == NFD_ERROR) return NFD_ERROR;
        if (!dbus_message_iter_next(&sub_iter)) break;
    }
    return NFD_OKAY;
}

// Read the message.  If response was okay, then returns NFD_OKAY and set file to it (the pointer is
// set to some string owned by msg, so you should not manually free it). Otherwise, returns
// NFD_CANCEL or NFD_ERROR as appropriate, and does not modify `file`.
nfdresult_t ReadResponseParamsSingle(DBusMessage* msg, const char*& file) {
    DBusMessageIter iter;
    if (!dbus_message_iter_init(msg, &iter)) {
        NFDi_SetError("D-Bus response signal is missing one or more arguments.");
        return NFD_ERROR;
    }
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32) {
        NFDi_SetError("D-Bus response signal argument is not a uint32.");
        return NFD_ERROR;
    }
    dbus_uint32_t resp_code;
    dbus_message_iter_get_basic(&iter, &resp_code);
    if (resp_code != 0) {
        if (resp_code == 1) {
            // User pressed cancel
            return NFD_CANCEL;
        } else {
            // Some error occurred
            NFDi_SetError("D-Bus file dialog interaction was ended abruptly.");
            return NFD_ERROR;
        }
    }
    // User successfully responded
    if (!dbus_message_iter_next(&iter)) {
        NFDi_SetError("D-Bus response signal is missing one or more arguments.");
        return NFD_ERROR;
    }
    if (ReadDict(iter, "uris", [&file](DBusMessageIter& uris_iter) {
            if (dbus_message_iter_get_arg_type(&uris_iter) != DBUS_TYPE_ARRAY) {
                NFDi_SetError("D-Bus response signal URI iter is not an array.");
                return NFD_ERROR;
            }
            DBusMessageIter uris_sub_iter;
            dbus_message_iter_recurse(&uris_iter, &uris_sub_iter);
            if (dbus_message_iter_get_arg_type(&uris_sub_iter) != DBUS_TYPE_STRING) {
                NFDi_SetError("D-Bus response signal URI sub iter is not an string.");
                return NFD_ERROR;
            }
            dbus_message_iter_get_basic(&uris_sub_iter, &file);
            return NFD_OKAY;
        }) == NFD_ERROR)
        return NFD_ERROR;

    return NFD_OKAY;
}

// char* ConvertConnectionName(const char* dbus_unique_name){
//     if (*dbus_unique_name == ':') ++dbus_unique_name;
//     char* converted = NFDi_Malloc<char>(strlen(dbus_unique_name) + 1);
//     char* out_ptr = converted;
//     for ()
// }

// Does not own the filter and extension.
// struct Pair_GtkFileFilter_FileExtension {
//     GtkFileFilter* filter;
//     const nfdnchar_t* extensionBegin;
//     const nfdnchar_t* extensionEnd;
// };

// struct ButtonClickedArgs {
//     Pair_GtkFileFilter_FileExtension* map;
//     GtkFileChooser* chooser;
// };

// void AddFiltersToDialog(GtkFileChooser* chooser,
//                         const nfdnfilteritem_t* filterList,
//                         nfdfiltersize_t filterCount) {
//     if (filterCount) {
//         assert(filterList);

//         // we have filters to add ... format and add them

//         for (nfdfiltersize_t index = 0; index != filterCount; ++index) {
//             GtkFileFilter* filter = gtk_file_filter_new();

//             // count number of file extensions
//             size_t sep = 1;
//             for (const nfdnchar_t* p_spec = filterList[index].spec; *p_spec; ++p_spec) {
//                 if (*p_spec == L',') {
//                     ++sep;
//                 }
//             }

//             // friendly name conversions: "png,jpg" -> "Image files
//             // (png, jpg)"

//             // calculate space needed (including the trailing '\0')
//             size_t nameSize =
//                 sep + strlen(filterList[index].spec) + 3 + strlen(filterList[index].name);

//             // malloc the required memory
//             nfdnchar_t* nameBuf = NFDi_Malloc<nfdnchar_t>(sizeof(nfdnchar_t) * nameSize);

//             nfdnchar_t* p_nameBuf = nameBuf;
//             for (const nfdnchar_t* p_filterName = filterList[index].name; *p_filterName;
//                  ++p_filterName) {
//                 *p_nameBuf++ = *p_filterName;
//             }
//             *p_nameBuf++ = ' ';
//             *p_nameBuf++ = '(';
//             const nfdnchar_t* p_extensionStart = filterList[index].spec;
//             for (const nfdnchar_t* p_spec = filterList[index].spec; true; ++p_spec) {
//                 if (*p_spec == ',' || !*p_spec) {
//                     if (*p_spec == ',') {
//                         *p_nameBuf++ = ',';
//                         *p_nameBuf++ = ' ';
//                     }

//                     // +1 for the trailing '\0'
//                     nfdnchar_t* extnBuf = NFDi_Malloc<nfdnchar_t>(sizeof(nfdnchar_t) *
//                                                                   (p_spec - p_extensionStart +
//                                                                   3));
//                     nfdnchar_t* p_extnBufEnd = extnBuf;
//                     *p_extnBufEnd++ = '*';
//                     *p_extnBufEnd++ = '.';
//                     p_extnBufEnd = copy(p_extensionStart, p_spec, p_extnBufEnd);
//                     *p_extnBufEnd++ = '\0';
//                     assert((size_t)(p_extnBufEnd - extnBuf) ==
//                            sizeof(nfdnchar_t) * (p_spec - p_extensionStart + 3));
//                     gtk_file_filter_add_pattern(filter, extnBuf);
//                     NFDi_Free(extnBuf);

//                     if (*p_spec) {
//                         // update the extension start point
//                         p_extensionStart = p_spec + 1;
//                     } else {
//                         // reached the '\0' character
//                         break;
//                     }
//                 } else {
//                     *p_nameBuf++ = *p_spec;
//                 }
//             }
//             *p_nameBuf++ = ')';
//             *p_nameBuf++ = '\0';
//             assert((size_t)(p_nameBuf - nameBuf) == sizeof(nfdnchar_t) * nameSize);

//             // add to the filter
//             gtk_file_filter_set_name(filter, nameBuf);

//             // free the memory
//             NFDi_Free(nameBuf);

//             // add filter to chooser
//             gtk_file_chooser_add_filter(chooser, filter);
//         }
//     }

//     /* always append a wildcard option to the end*/

//     GtkFileFilter* filter = gtk_file_filter_new();
//     gtk_file_filter_set_name(filter, "All files");
//     gtk_file_filter_add_pattern(filter, "*");
//     gtk_file_chooser_add_filter(chooser, filter);
// }

// // returns null-terminated map (trailing .filter is null)
// Pair_GtkFileFilter_FileExtension* AddFiltersToDialogWithMap(GtkFileChooser* chooser,
//                                                             const nfdnfilteritem_t* filterList,
//                                                             nfdfiltersize_t filterCount) {
//     Pair_GtkFileFilter_FileExtension* map = NFDi_Malloc<Pair_GtkFileFilter_FileExtension>(
//         sizeof(Pair_GtkFileFilter_FileExtension) * (filterCount + 1));

//     if (filterCount) {
//         assert(filterList);

//         // we have filters to add ... format and add them

//         for (nfdfiltersize_t index = 0; index != filterCount; ++index) {
//             GtkFileFilter* filter = gtk_file_filter_new();

//             // store filter in map
//             map[index].filter = filter;
//             map[index].extensionBegin = filterList[index].spec;
//             map[index].extensionEnd = nullptr;

//             // count number of file extensions
//             size_t sep = 1;
//             for (const nfdnchar_t* p_spec = filterList[index].spec; *p_spec; ++p_spec) {
//                 if (*p_spec == L',') {
//                     ++sep;
//                 }
//             }

//             // friendly name conversions: "png,jpg" -> "Image files
//             // (png, jpg)"

//             // calculate space needed (including the trailing '\0')
//             size_t nameSize =
//                 sep + strlen(filterList[index].spec) + 3 + strlen(filterList[index].name);

//             // malloc the required memory
//             nfdnchar_t* nameBuf = NFDi_Malloc<nfdnchar_t>(sizeof(nfdnchar_t) * nameSize);

//             nfdnchar_t* p_nameBuf = nameBuf;
//             for (const nfdnchar_t* p_filterName = filterList[index].name; *p_filterName;
//                  ++p_filterName) {
//                 *p_nameBuf++ = *p_filterName;
//             }
//             *p_nameBuf++ = ' ';
//             *p_nameBuf++ = '(';
//             const nfdnchar_t* p_extensionStart = filterList[index].spec;
//             for (const nfdnchar_t* p_spec = filterList[index].spec; true; ++p_spec) {
//                 if (*p_spec == ',' || !*p_spec) {
//                     if (*p_spec == ',') {
//                         *p_nameBuf++ = ',';
//                         *p_nameBuf++ = ' ';
//                     }

//                     // +1 for the trailing '\0'
//                     nfdnchar_t* extnBuf = NFDi_Malloc<nfdnchar_t>(sizeof(nfdnchar_t) *
//                                                                   (p_spec - p_extensionStart +
//                                                                   3));
//                     nfdnchar_t* p_extnBufEnd = extnBuf;
//                     *p_extnBufEnd++ = '*';
//                     *p_extnBufEnd++ = '.';
//                     p_extnBufEnd = copy(p_extensionStart, p_spec, p_extnBufEnd);
//                     *p_extnBufEnd++ = '\0';
//                     assert((size_t)(p_extnBufEnd - extnBuf) ==
//                            sizeof(nfdnchar_t) * (p_spec - p_extensionStart + 3));
//                     gtk_file_filter_add_pattern(filter, extnBuf);
//                     NFDi_Free(extnBuf);

//                     // store current pointer in map (if it's
//                     // the first one)
//                     if (map[index].extensionEnd == nullptr) {
//                         map[index].extensionEnd = p_spec;
//                     }

//                     if (*p_spec) {
//                         // update the extension start point
//                         p_extensionStart = p_spec + 1;
//                     } else {
//                         // reached the '\0' character
//                         break;
//                     }
//                 } else {
//                     *p_nameBuf++ = *p_spec;
//                 }
//             }
//             *p_nameBuf++ = ')';
//             *p_nameBuf++ = '\0';
//             assert((size_t)(p_nameBuf - nameBuf) == sizeof(nfdnchar_t) * nameSize);

//             // add to the filter
//             gtk_file_filter_set_name(filter, nameBuf);

//             // free the memory
//             NFDi_Free(nameBuf);

//             // add filter to chooser
//             gtk_file_chooser_add_filter(chooser, filter);
//         }
//     }
//     // set trailing map index to null
//     map[filterCount].filter = nullptr;

//     /* always append a wildcard option to the end*/
//     GtkFileFilter* filter = gtk_file_filter_new();
//     gtk_file_filter_set_name(filter, "All files");
//     gtk_file_filter_add_pattern(filter, "*");
//     gtk_file_chooser_add_filter(chooser, filter);

//     return map;
// }

// void SetDefaultPath(GtkFileChooser* chooser, const char* defaultPath) {
//     if (!defaultPath || !*defaultPath) return;

//     /* GTK+ manual recommends not specifically setting the default path.
//     We do it anyway in order to be consistent across platforms.

//     If consistency with the native OS is preferred, this is the line
//     to comment out. -ml */
//     gtk_file_chooser_set_current_folder(chooser, defaultPath);
// }

// void SetDefaultName(GtkFileChooser* chooser, const char* defaultName) {
//     if (!defaultName || !*defaultName) return;

//     gtk_file_chooser_set_current_name(chooser, defaultName);
// }

// void WaitForCleanup() {
//     while (gtk_events_pending()) gtk_main_iteration();
// }

// struct Widget_Guard {
//     GtkWidget* data;
//     Widget_Guard(GtkWidget* widget) : data(widget) {}
//     ~Widget_Guard() {
//         WaitForCleanup();
//         gtk_widget_destroy(data);
//         WaitForCleanup();
//     }
// };

// void FileActivatedSignalHandler(GtkButton* saveButton, void* userdata) {
//     (void)saveButton;  // silence the unused arg warning

//     ButtonClickedArgs* args = static_cast<ButtonClickedArgs*>(userdata);
//     GtkFileChooser* chooser = args->chooser;
//     char* currentFileName = gtk_file_chooser_get_current_name(chooser);
//     if (*currentFileName) {  // string is not empty

//         // find a '.' in the file name
//         const char* p_period = currentFileName;
//         for (; *p_period; ++p_period) {
//             if (*p_period == '.') {
//                 break;
//             }
//         }

//         if (!*p_period) {  // there is no '.', so append the default extension
//             Pair_GtkFileFilter_FileExtension* filterMap =
//                 static_cast<Pair_GtkFileFilter_FileExtension*>(args->map);
//             GtkFileFilter* currentFilter = gtk_file_chooser_get_filter(chooser);
//             if (currentFilter) {
//                 for (; filterMap->filter; ++filterMap) {
//                     if (filterMap->filter == currentFilter) break;
//                 }
//             }
//             if (filterMap->filter) {
//                 // memory for appended string (including '.' and
//                 // trailing '\0')
//                 char* appendedFileName = NFDi_Malloc<char>(
//                     sizeof(char) * ((p_period - currentFileName) +
//                                     (filterMap->extensionEnd - filterMap->extensionBegin) + 2));
//                 char* p_fileName = copy(currentFileName, p_period, appendedFileName);
//                 *p_fileName++ = '.';
//                 p_fileName = copy(filterMap->extensionBegin, filterMap->extensionEnd,
//                 p_fileName); *p_fileName++ = '\0';

//                 assert(p_fileName - appendedFileName ==
//                        (p_period - currentFileName) +
//                            (filterMap->extensionEnd - filterMap->extensionBegin) + 2);

//                 // set the appended file name
//                 gtk_file_chooser_set_current_name(chooser, appendedFileName);

//                 // free the memory
//                 NFDi_Free(appendedFileName);
//             }
//         }
//     }

//     // free the memory
//     g_free(currentFileName);
// }

// // wrapper for gtk_dialog_run() that brings the dialog to the front
// // see issues at:
// // https://github.com/btzy/nativefiledialog-extended/issues/31
// // https://github.com/mlabbe/nativefiledialog/pull/92
// // https://github.com/guillaumechereau/noc/pull/11
// gint RunDialogWithFocus(GtkDialog* dialog) {
// #if defined(GDK_WINDOWING_X11)
//     gtk_widget_show_all(GTK_WIDGET(dialog));  // show the dialog so that it gets a display
//     if (GDK_IS_X11_DISPLAY(gtk_widget_get_display(GTK_WIDGET(dialog)))) {
//         GdkWindow* window = gtk_widget_get_window(GTK_WIDGET(dialog));
//         gdk_window_set_events(
//             window,
//             static_cast<GdkEventMask>(gdk_window_get_events(window) | GDK_PROPERTY_CHANGE_MASK));
//         gtk_window_present_with_time(GTK_WINDOW(dialog), gdk_x11_get_server_time(window));
//     }
// #endif
//     return gtk_dialog_run(dialog);
// }

// Appends up to 64 random chars to the given pointer.  Returns the end of the appended chars.
char* Generate64RandomChars(char* out) {
    size_t amount = 32;
    while (amount > 0) {
        unsigned char buf[32];
        ssize_t res = getrandom(buf, amount, 0);
        if (res == -1) {
            if (errno == EINTR)
                continue;
            else
                break;  // too bad, urandom isn't working well
        }
        amount -= res;
        // we encode each random char using two chars, since they must be [A-Z][a-z][0-9]_
        for (size_t i = 0; i != static_cast<size_t>(res); ++i) {
            *out++ = 'A' + static_cast<char>(buf[i] & 15);
            *out++ = 'A' + static_cast<char>(buf[i] >> 4);
        }
    }
    return out;
}

constexpr const char STR_RESPONSE_HANDLE_PREFIX[] = "/org/freedesktop/portal/desktop/request/";
constexpr size_t STR_RESPONSE_HANDLE_PREFIX_LEN =
    sizeof(STR_RESPONSE_HANDLE_PREFIX) - 1;  // -1 to remove the \0.

// Allocates and returns a path like "/org/freedesktop/portal/desktop/request/SENDER/TOKEN" with
// randomly generated TOKEN as recommended by flatpak.  `handle_token_ptr` is a poinnter to the
// TOKEN part.
char* MakeUniqueObjectPath(const char** handle_token_ptr) {
    const char* sender = dbus_unique_name;
    if (*sender == ':') ++sender;
    const size_t sender_len = strlen(sender);
    const size_t sz = STR_RESPONSE_HANDLE_PREFIX_LEN + sender_len + 1 +
                      64;  // 1 for '/', followed by 64 random chars
    char* path = NFDi_Malloc<char>(sz + 1);
    char* path_ptr = path;
    path_ptr = copy(STR_RESPONSE_HANDLE_PREFIX,
                    STR_RESPONSE_HANDLE_PREFIX + STR_RESPONSE_HANDLE_PREFIX_LEN,
                    path_ptr);
    path_ptr = transform(
        sender, sender + sender_len, path_ptr, [](char ch) { return ch != '.' ? ch : '_'; });
    *path_ptr++ = '/';
    *handle_token_ptr = path_ptr;
    path_ptr = Generate64RandomChars(path_ptr);
    *path_ptr = '\0';
    return path;
}

class DBusSignalSubscriptionHandler {
   private:
    char* sub_cmd;

   public:
    DBusSignalSubscriptionHandler() : sub_cmd(nullptr) {}
    ~DBusSignalSubscriptionHandler() {
        if (sub_cmd) Unsubscribe();
    }

    nfdresult_t Subscribe(const char* handle_path) {
        if (sub_cmd) Unsubscribe();
        sub_cmd = MakeResponseSubscriptionPath(handle_path, dbus_unique_name);
        DBusError err;
        dbus_error_init(&err);
        dbus_bus_add_match(dbus_conn, sub_cmd, &err);
        if (dbus_error_is_set(&err)) {
            dbus_error_free(&dbus_err);
            dbus_move_error(&err, &dbus_err);
            NFDi_SetError(dbus_err.message);
            return NFD_ERROR;
        }
        return NFD_OKAY;
    }

    void Unsubscribe() {
        DBusError err;
        dbus_error_init(&err);
        dbus_bus_remove_match(dbus_conn, sub_cmd, &err);
        NFDi_Free(sub_cmd);
        sub_cmd = nullptr;
        dbus_error_free(
            &err);  // silence unsubscribe errors, because this is intuitively part of 'cleanup'
    }

   private:
    constexpr static const char STR_RESPONSE_SUBSCRIPTION_PATH_1[] =
        "type='signal',sender='org.freedesktop.portal.Desktop',path='";
    constexpr static const char STR_RESPONSE_SUBSCRIPTION_PATH_1_LEN =
        sizeof(STR_RESPONSE_SUBSCRIPTION_PATH_1) - 1;
    constexpr static const char STR_RESPONSE_SUBSCRIPTION_PATH_2[] =
        "',interface='org.freedesktop.portal.Request',member='Response',destination='";
    constexpr static const char STR_RESPONSE_SUBSCRIPTION_PATH_2_LEN =
        sizeof(STR_RESPONSE_SUBSCRIPTION_PATH_2) - 1;
    constexpr static const char STR_RESPONSE_SUBSCRIPTION_PATH_3[] = "'";
    constexpr static const char STR_RESPONSE_SUBSCRIPTION_PATH_3_LEN =
        sizeof(STR_RESPONSE_SUBSCRIPTION_PATH_3) - 1;

    static char* MakeResponseSubscriptionPath(const char* handle_path, const char* unique_name) {
        const size_t handle_path_len = strlen(handle_path);
        const size_t unique_name_len = strlen(unique_name);
        const size_t sz = STR_RESPONSE_SUBSCRIPTION_PATH_1_LEN + handle_path_len +
                          STR_RESPONSE_SUBSCRIPTION_PATH_2_LEN + unique_name_len +
                          STR_RESPONSE_SUBSCRIPTION_PATH_3_LEN;
        char* res = NFDi_Malloc<char>(sz + 1);
        char* res_ptr = res;
        res_ptr = copy(STR_RESPONSE_SUBSCRIPTION_PATH_1,
                       STR_RESPONSE_SUBSCRIPTION_PATH_1 + STR_RESPONSE_SUBSCRIPTION_PATH_1_LEN,
                       res_ptr);
        res_ptr = copy(handle_path, handle_path + handle_path_len, res_ptr);
        res_ptr = copy(STR_RESPONSE_SUBSCRIPTION_PATH_2,
                       STR_RESPONSE_SUBSCRIPTION_PATH_2 + STR_RESPONSE_SUBSCRIPTION_PATH_2_LEN,
                       res_ptr);
        res_ptr = copy(unique_name, unique_name + unique_name_len, res_ptr);
        res_ptr = copy(STR_RESPONSE_SUBSCRIPTION_PATH_3,
                       STR_RESPONSE_SUBSCRIPTION_PATH_3 + STR_RESPONSE_SUBSCRIPTION_PATH_3_LEN,
                       res_ptr);
        *res_ptr = '\0';
        return res;
    }
};

constexpr const char FILE_URI_PREFIX[] = "file://";
constexpr size_t FILE_URI_PREFIX_LEN = sizeof(FILE_URI_PREFIX) - 1;

// If fileUri starts with "file://", strips that prefix and copies it to a new buffer, and make outPath point to it, and returns NFD_OKAY.
// Otherwise, does not modify outPath and returns NFD_ERROR (with the correct error set)
nfdresult_t AllocAndCopyFIlePath(const char* fileUri, char*& outPath) {
    const char* prefix_begin = FILE_URI_PREFIX;
    const char* const prefix_end = FILE_URI_PREFIX + FILE_URI_PREFIX_LEN;
    for (; prefix_begin != prefix_end; ++prefix_begin, ++fileUri) {
        if (*prefix_begin != *fileUri) {
            NFDi_SetError("D-Bus freedesktop portal returned a URI that is not a file URI.");
            return NFD_ERROR;
        }
    }
    size_t len = strlen(fileUri);
    char* path_without_prefix = NFDi_Malloc<char>(len + 1);
    copy(fileUri, fileUri + (len + 1), path_without_prefix);
    outPath = path_without_prefix;
    return NFD_OKAY;
}

}  // namespace

/* public */

const char* NFD_GetError(void) {
    return err_ptr;
}

void NFD_ClearError(void) {
    NFDi_SetError(nullptr);
    dbus_error_free(&dbus_err);
}

nfdresult_t NFD_Init(void) {
    // Initialize dbus_error
    dbus_error_init(&dbus_err);
    // Get DBus connection
    dbus_conn = dbus_bus_get(DBUS_BUS_SESSION, &dbus_err);
    if (!dbus_conn) {
        NFDi_SetError(dbus_err.message);
        return NFD_ERROR;
    }
    dbus_unique_name = dbus_bus_get_unique_name(dbus_conn);
    if (!dbus_unique_name) {
        NFDi_SetError("Unable to get the unique name of our D-Bus connection.");
        return NFD_ERROR;
    }
    return NFD_OKAY;
}
void NFD_Quit(void) {
    dbus_connection_unref(dbus_conn);
    // Note: We do not free dbus_error since NFD_Init might set it.
    // To avoid leaking memory, the caller should explicitly call NFD_ClearError after reading the
    // error.
}

void NFD_FreePathN(nfdnchar_t* filePath) {
    assert(filePath);
    NFDi_Free(filePath);
}

nfdresult_t NFD_OpenDialogN(nfdnchar_t** outPath,
                            const nfdnfilteritem_t* filterList,
                            nfdfiltersize_t filterCount,
                            const nfdnchar_t* defaultPath) {
    const char* handle_token_ptr;
    char* handle_obj_path = MakeUniqueObjectPath(&handle_token_ptr);
    Free_Guard<char> handle_obj_path_guard(handle_obj_path);

    DBusError err;  // need a separate error object because we don't want to mess with the old one
                    // if it's stil set
    dbus_error_init(&err);

    // Subscribe to the signal using the handle_obj_path
    DBusSignalSubscriptionHandler signal_sub;
    nfdresult_t res = signal_sub.Subscribe(handle_obj_path);
    if (res != NFD_OKAY) return res;

    // TODO: use XOpenDisplay()/XGetInputFocus() to find xid of window... but what should one do on
    // Wayland?

    DBusMessage* query = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                      "/org/freedesktop/portal/desktop",
                                                      "org.freedesktop.portal.FileChooser",
                                                      "OpenFile");
    DBusMessage_Guard query_guard(query);
    AppendOpenFileQueryParams<false>(query, handle_token_ptr, filterList, filterCount);

    DBusMessage* reply =
        dbus_connection_send_with_reply_and_block(dbus_conn, query, DBUS_TIMEOUT_INFINITE, &err);
    if (!reply) {
        dbus_error_free(&dbus_err);
        dbus_move_error(&err, &dbus_err);
        NFDi_SetError(dbus_err.message);
        return NFD_ERROR;
    }
    DBusMessage_Guard reply_guard(reply);

    // Check the reply and update our signal subscription if necessary
    {
        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter)) {
            NFDi_SetError("D-Bus reply is missing an argument.");
            return NFD_ERROR;
        }
        if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH) {
            NFDi_SetError("D-Bus reply is not an object path.");
            return NFD_ERROR;
        }

        const char* path;
        dbus_message_iter_get_basic(&iter, &path);
        if (strcmp(path, handle_obj_path) != 0) {
            // needs to change our signal subscription
            signal_sub.Subscribe(path);
        }
    }

    // Wait and read the response
    const char* file = nullptr;
    do {
        while (true) {
            DBusMessage* msg = dbus_connection_pop_message(dbus_conn);
            if (!msg) break;
            DBusMessage_Guard msg_guard(msg);

            if (dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response")) {
                // this is the response we're looking for
                const nfdresult_t res = ReadResponseParamsSingle(msg, file);
                if (res != NFD_OKAY) {
                    return res;
                }
                break;
            }
        }
        if (file) break;
    } while (dbus_connection_read_write(dbus_conn, -1));

    if (!file) {
        NFDi_SetError("D-Bus freedesktop portal did not give us a reply.");
        return NFD_ERROR;
    }

    {
        const nfdresult_t res = AllocAndCopyFIlePath(file, *outPath);
        if (res != NFD_OKAY) {
            return res;
        }
    }

    return NFD_OKAY;

    (void)defaultPath; // Not supported for portal backend
}

// nfdresult_t NFD_OpenDialogN(nfdnchar_t** outPath,
//                             const nfdnfilteritem_t* filterList,
//                             nfdfiltersize_t filterCount,
//                             const nfdnchar_t* defaultPath) {
//     GtkWidget* widget = gtk_file_chooser_dialog_new("Open File",
//                                                     nullptr,
//                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
//                                                     "_Cancel",
//                                                     GTK_RESPONSE_CANCEL,
//                                                     "_Open",
//                                                     GTK_RESPONSE_ACCEPT,
//                                                     nullptr);

//     // guard to destroy the widget when returning from this function
//     Widget_Guard widgetGuard(widget);

//     /* Build the filter list */
//     AddFiltersToDialog(GTK_FILE_CHOOSER(widget), filterList, filterCount);

//     /* Set the default path */
//     SetDefaultPath(GTK_FILE_CHOOSER(widget), defaultPath);

//     if (RunDialogWithFocus(GTK_DIALOG(widget)) == GTK_RESPONSE_ACCEPT) {
//         // write out the file name
//         *outPath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));

//         return NFD_OKAY;
//     } else {
//         return NFD_CANCEL;
//     }
// }

// nfdresult_t NFD_OpenDialogMultipleN(const nfdpathset_t** outPaths,
//                                     const nfdnfilteritem_t* filterList,
//                                     nfdfiltersize_t filterCount,
//                                     const nfdnchar_t* defaultPath) {
//     GtkWidget* widget = gtk_file_chooser_dialog_new("Open Files",
//                                                     nullptr,
//                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
//                                                     "_Cancel",
//                                                     GTK_RESPONSE_CANCEL,
//                                                     "_Open",
//                                                     GTK_RESPONSE_ACCEPT,
//                                                     nullptr);

//     // guard to destroy the widget when returning from this function
//     Widget_Guard widgetGuard(widget);

//     // set select multiple
//     gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(widget), TRUE);

//     /* Build the filter list */
//     AddFiltersToDialog(GTK_FILE_CHOOSER(widget), filterList, filterCount);

//     /* Set the default path */
//     SetDefaultPath(GTK_FILE_CHOOSER(widget), defaultPath);

//     if (RunDialogWithFocus(GTK_DIALOG(widget)) == GTK_RESPONSE_ACCEPT) {
//         // write out the file name
//         GSList* fileList = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(widget));

//         *outPaths = static_cast<void*>(fileList);
//         return NFD_OKAY;
//     } else {
//         return NFD_CANCEL;
//     }
// }

// nfdresult_t NFD_SaveDialogN(nfdnchar_t** outPath,
//                             const nfdnfilteritem_t* filterList,
//                             nfdfiltersize_t filterCount,
//                             const nfdnchar_t* defaultPath,
//                             const nfdnchar_t* defaultName) {
//     GtkWidget* widget = gtk_file_chooser_dialog_new("Save File",
//                                                     nullptr,
//                                                     GTK_FILE_CHOOSER_ACTION_SAVE,
//                                                     "_Cancel",
//                                                     GTK_RESPONSE_CANCEL,
//                                                     nullptr);

//     // guard to destroy the widget when returning from this function
//     Widget_Guard widgetGuard(widget);

//     GtkWidget* saveButton = gtk_dialog_add_button(GTK_DIALOG(widget), "_Save",
//     GTK_RESPONSE_ACCEPT);

//     // Prompt on overwrite
//     gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(widget), TRUE);

//     /* Build the filter list */
//     ButtonClickedArgs buttonClickedArgs;
//     buttonClickedArgs.chooser = GTK_FILE_CHOOSER(widget);
//     buttonClickedArgs.map =
//         AddFiltersToDialogWithMap(GTK_FILE_CHOOSER(widget), filterList, filterCount);

//     /* Set the default path */
//     SetDefaultPath(GTK_FILE_CHOOSER(widget), defaultPath);

//     /* Set the default file name */
//     SetDefaultName(GTK_FILE_CHOOSER(widget), defaultName);

//     /* set the handler to add file extension */
//     gulong handlerID = g_signal_connect(G_OBJECT(saveButton),
//                                         "pressed",
//                                         G_CALLBACK(FileActivatedSignalHandler),
//                                         static_cast<void*>(&buttonClickedArgs));

//     /* invoke the dialog (blocks until dialog is closed) */
//     gint result = RunDialogWithFocus(GTK_DIALOG(widget));
//     /* unset the handler */
//     g_signal_handler_disconnect(G_OBJECT(saveButton), handlerID);

//     /* free the filter map */
//     NFDi_Free(buttonClickedArgs.map);

//     if (result == GTK_RESPONSE_ACCEPT) {
//         // write out the file name
//         *outPath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));

//         return NFD_OKAY;
//     } else {
//         return NFD_CANCEL;
//     }
// }

// nfdresult_t NFD_PickFolderN(nfdnchar_t** outPath, const nfdnchar_t* defaultPath) {
//     GtkWidget* widget = gtk_file_chooser_dialog_new("Select folder",
//                                                     nullptr,
//                                                     GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
//                                                     "_Cancel",
//                                                     GTK_RESPONSE_CANCEL,
//                                                     "_Select",
//                                                     GTK_RESPONSE_ACCEPT,
//                                                     nullptr);

//     // guard to destroy the widget when returning from this function
//     Widget_Guard widgetGuard(widget);

//     /* Set the default path */
//     SetDefaultPath(GTK_FILE_CHOOSER(widget), defaultPath);

//     if (RunDialogWithFocus(GTK_DIALOG(widget)) == GTK_RESPONSE_ACCEPT) {
//         // write out the file name
//         *outPath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));

//         return NFD_OKAY;
//     } else {
//         return NFD_CANCEL;
//     }
// }

// nfdresult_t NFD_PathSet_GetCount(const nfdpathset_t* pathSet, nfdpathsetsize_t* count) {
//     assert(pathSet);
//     // const_cast because methods on GSList aren't const, but it should act
//     // like const to the caller
//     GSList* fileList = const_cast<GSList*>(static_cast<const GSList*>(pathSet));

//     *count = g_slist_length(fileList);
//     return NFD_OKAY;
// }

// nfdresult_t NFD_PathSet_GetPathN(const nfdpathset_t* pathSet,
//                                  nfdpathsetsize_t index,
//                                  nfdnchar_t** outPath) {
//     assert(pathSet);
//     // const_cast because methods on GSList aren't const, but it should act
//     // like const to the caller
//     GSList* fileList = const_cast<GSList*>(static_cast<const GSList*>(pathSet));

//     // Note: this takes linear time... but should be good enough
//     *outPath = static_cast<nfdnchar_t*>(g_slist_nth_data(fileList, index));

//     return NFD_OKAY;
// }

// void NFD_PathSet_FreePathN(const nfdnchar_t* filePath) {
//     assert(filePath);
//     // no-op, because NFD_PathSet_Free does the freeing for us
// }

// void NFD_PathSet_Free(const nfdpathset_t* pathSet) {
//     assert(pathSet);
//     // const_cast because methods on GSList aren't const, but it should act
//     // like const to the caller
//     GSList* fileList = const_cast<GSList*>(static_cast<const GSList*>(pathSet));

//     // free all the nodes
//     for (GSList* node = fileList; node; node = node->next) {
//         assert(node->data);
//         g_free(node->data);
//     }

//     // free the path set memory
//     g_slist_free(fileList);
// }

// nfdresult_t NFD_PathSet_GetEnum(const nfdpathset_t* pathSet, nfdpathsetenum_t* outEnumerator) {
//     // The pathset (GSList) is already a linked list, so the enumeration is itself
//     outEnumerator->ptr = const_cast<void*>(pathSet);

//     return NFD_OKAY;
// }

// void NFD_PathSet_FreeEnum(nfdpathsetenum_t*) {
//     // Do nothing, because the enumeration is the pathset itself
// }

// nfdresult_t NFD_PathSet_EnumNextN(nfdpathsetenum_t* enumerator, nfdnchar_t** outPath) {
//     const GSList* fileList = static_cast<const GSList*>(enumerator->ptr);

//     if (fileList) {
//         *outPath = static_cast<nfdnchar_t*>(fileList->data);
//         enumerator->ptr = static_cast<void*>(fileList->next);
//     } else {
//         *outPath = nullptr;
//     }

//     return NFD_OKAY;
// }
