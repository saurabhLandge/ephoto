#include "ephoto.h"

#define FILESEP             "file://"
#define FILESEP_LEN         sizeof(FILESEP) - 1

#define TODO_ITEM_MIN_BATCH 5

#define ANIM_TIME           0.2

typedef struct _Ephoto_Directory_Browser Ephoto_Directory_Browser;

struct _Ephoto_Directory_Browser
{
   Ephoto *ephoto;
   Evas_Object *main;
   Evas_Object *fsel;
   Evas_Object *fsel_back;
   Evas_Object *leftbox;
   Elm_Object_Item *dir_current;
   Elm_Object_Item *last_sel;
   Eio_File *ls;
   Eina_Bool dirs_only;
   Eina_Bool thumbs_only;
   Ecore_File_Monitor *monitor;
   Eina_List *handlers;
   Eina_List *todo_items;
   Ecore_Job *change_dir_job;
   Ecore_Timer *click_timer;
   Eina_Bool dragging;
   Eina_Bool processing;
   Eina_Bool initializing;
   struct
   {
      Ecore_Animator *todo_items;
      int count;
      int processed;
   } animator;
   Eina_Bool main_deleted:1;
   const char *back_directory;
};

static Elm_Genlist_Item_Class _ephoto_dir_class;

static char * _drag_data_extract(char **drag_data);

static void _fsel_mouse_up_cb(void *data, Evas *e EINA_UNUSED,
    Evas_Object *obj EINA_UNUSED, void *event_info);

/*File Pane Callbacks*/
static void
_menu_dismissed_cb(void *data, Evas_Object *obj,
    void *event_info EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;

   db->ephoto->menu_blocking = EINA_FALSE;
   evas_object_del(obj);
   elm_object_focus_set(db->main, EINA_TRUE);
}

static void
_menu_empty_cb(void *data, Evas_Object *obj EINA_UNUSED,
    void *event_info EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;
   Eina_List *paths = NULL;
   Elm_Object_Item *item;
   Ephoto_Entry *file;
   
   item = elm_genlist_first_item_get(db->fsel);
   while (item)
     {
        file = elm_object_item_data_get(item);
        paths = eina_list_append(paths, strdup(file->path));
        item = elm_genlist_item_next_get(item);
     }
   if (eina_list_count(paths) <= 0)
     return;
   ephoto_file_empty_trash(db->ephoto, paths);
}

static Eina_Bool
_drop_dropcb(void *data EINA_UNUSED, Evas_Object *obj, Elm_Object_Item *it,
    Elm_Selection_Data *ev, int xposret EINA_UNUSED, int yposret EINA_UNUSED)
{
   EINA_SAFETY_ON_NULL_RETURN_VAL(it, EINA_TRUE);

   Ephoto_Entry *entry = elm_object_item_data_get(it);
   const char *path = entry->path;
   Eina_List *files = NULL;
   Ephoto_Directory_Browser *db = evas_object_data_get(obj, "directory_browser");

   if (!ev->data)
      return EINA_FALSE;
   if (ev->len <= 0)
      return EINA_FALSE;
   if (!path)
      return EINA_FALSE;

   char *dd = strdup(ev->data);

   if (!dd)
      return EINA_FALSE;

   char *s = _drag_data_extract(&dd);

   while (s)
     {
	files = eina_list_append(files, s);
	s = _drag_data_extract(&dd);
     }
   free(dd);

   if (db->ephoto->config->move_drop)
     ephoto_file_move(db->ephoto, files, path);
   else
     ephoto_file_copy(db->ephoto, files, path);
   if (db->dir_current)
     elm_genlist_item_selected_set(db->dir_current, EINA_TRUE);
   return EINA_TRUE;
}

static Elm_Object_Item *
_drop_item_getcb(Evas_Object *obj, Evas_Coord x, Evas_Coord y,
    int *xposret EINA_UNUSED, int *yposret)
{
   Elm_Object_Item *gli;

   gli = elm_genlist_at_xy_item_get(obj, x, y, yposret);

   return gli;
}

static void
_drop_enter(void *data, Evas_Object *obj EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;

   if (db->dragging)
     elm_object_cursor_set(db->main, ELM_CURSOR_TARGET);
}

static void
_drop_leave(void *data, Evas_Object *obj EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;

   if (db->dragging)
     {
        elm_object_cursor_set(db->main, ELM_CURSOR_HAND2);
        if (db->dir_current)
          elm_genlist_item_selected_set(db->dir_current, EINA_TRUE);
     }
}

static void
_drop_pos(void *data EINA_UNUSED, Evas_Object *cont EINA_UNUSED,
    Elm_Object_Item *it EINA_UNUSED, Evas_Coord x EINA_UNUSED,
    Evas_Coord y EINA_UNUSED, int xposret EINA_UNUSED,
    int yposret EINA_UNUSED, Elm_Xdnd_Action action EINA_UNUSED)
{
   elm_genlist_item_selected_set(it, EINA_TRUE);
}

static char *
_drag_data_extract(char **drag_data)
{
   char *uri = NULL;

   if (!drag_data)
      return uri;

   char *p = *drag_data;

   if (!p)
      return uri;
   char *s = strstr(p, FILESEP);

   if (s)
      p += FILESEP_LEN;
   s = strchr(p, '\n');
   uri = p;
   if (s)
     {
	if (s - p > 0)
	  {
	     char *s1 = s - 1;

	     if (s1[0] == '\r')
		s1[0] = '\0';
	     else
	       {
		  char *s2 = s + 1;

		  if (s2[0] == '\r')
		    {
		       s[0] = '\0';
		       s++;
		    }
                  else
		     s[0] = '\0';
	       }
	  }
        else
	   s[0] = '\0';
	s++;
     }
   else
     p = NULL;
   *drag_data = s;

   return uri;
}

static int
_entry_cmp(const void *pa, const void *pb)
{
   const Ephoto_Entry *a, *b;

   a = elm_object_item_data_get(pa);
   b = elm_object_item_data_get(pb);

   return strcasecmp(a->basename, b->basename);
}

static void
_on_list_expand_req(void *data, Evas_Object *obj EINA_UNUSED,
    void *event_info)
{
   Ephoto_Directory_Browser *db = data;
   Elm_Object_Item *it = event_info;

   if (db->initializing)
     return;

   ecore_job_del(db->change_dir_job);
   db->change_dir_job = NULL;
   ecore_timer_del(db->click_timer);
   db->click_timer = NULL;
   elm_genlist_item_expanded_set(it, EINA_TRUE);
}

static void
_on_list_contract_req(void *data EINA_UNUSED, Evas_Object *obj EINA_UNUSED,
    void *event_info)
{
   Ephoto_Directory_Browser *db = data;
   Elm_Object_Item *it = event_info;

   if (db->initializing)
     return;

   ecore_job_del(db->change_dir_job);
   db->change_dir_job = NULL;
   ecore_timer_del(db->click_timer);
   db->click_timer = NULL;
   elm_genlist_item_expanded_set(it, EINA_FALSE);
}

static void
_on_list_expanded(void *data, Evas_Object *obj EINA_UNUSED, void *event_info)
{
   Ephoto_Directory_Browser *db = data;
   Elm_Object_Item *it = event_info;
   Ephoto_Entry *entry;
   const char *path;

   if (db->initializing)
     return;

   entry = elm_object_item_data_get(it);
   path = entry->path;
   db->dirs_only = 0;
   if (!strcmp(path, db->ephoto->config->directory))
     db->dirs_only = 1;
   else
     db->dirs_only = 0;
   db->thumbs_only = 0;
   ephoto_directory_set(db->ephoto, path, it, db->dirs_only, db->thumbs_only);
   ephoto_title_set(db->ephoto, db->ephoto->config->directory);
}

static void
_on_list_contracted(void *data, Evas_Object *obj EINA_UNUSED, void *event_info)
{
   Ephoto_Directory_Browser *db = data;
   Elm_Object_Item *it = event_info;
   Ephoto_Entry *entry;
   const char *path;

   if (db->initializing)
     return;

   entry = elm_object_item_data_get(it);
   path = entry->path;
   elm_genlist_item_subitems_clear(it);
   if (!strcmp(path, db->ephoto->config->directory))
     return;
   db->thumbs_only = 1;
   db->dirs_only = 0;
   ephoto_directory_set(db->ephoto, path, NULL,
       db->dirs_only, db->thumbs_only);
   ephoto_title_set(db->ephoto,
       db->ephoto->config->directory);
}

static void
_dir_job(void *data)
{
   Elm_Object_Item *it = data;
   Ephoto_Directory_Browser *db = evas_object_data_get(it, "directory_browser");
   Ephoto_Entry *entry;
   const char *path;

   entry = elm_object_item_data_get(it);
   path = entry->path;
   db->change_dir_job = NULL;
   db->thumbs_only = 1;
   db->dirs_only = 0;
   ephoto_directory_set(db->ephoto, path, NULL,
       db->dirs_only, db->thumbs_only);
   ephoto_title_set(db->ephoto, db->ephoto->config->directory);
}

static void
_wait_job(void *data)
{
   Elm_Object_Item *it = data;
   Ephoto_Directory_Browser *db = evas_object_data_get(it, "directory_browser");

   if (db->change_dir_job)
     ecore_job_del(db->change_dir_job);
   db->change_dir_job = ecore_job_add(_dir_job, it);
}

static void
_on_list_selected(void *data, Evas_Object *obj EINA_UNUSED,
    void *event_info)
{
   Ephoto_Directory_Browser *db = data;
   Elm_Object_Item *it = event_info;

   evas_object_data_set(it, "directory_browser", db);
   if (!db->dragging)
     {
        db->dir_current = it;

        ecore_job_add(_wait_job, it);
     }
}

static char *
_dir_item_text_get(void *data, Evas_Object *obj EINA_UNUSED,
    const char *part EINA_UNUSED)
{
   Ephoto_Entry *e = data;

   return strdup(e->label);
}

static Evas_Object *
_dir_item_icon_get(void *data EINA_UNUSED, Evas_Object *obj,
    const char *part)
{
   if (!strcmp(part, "elm.swallow.end"))
      return NULL;
   Evas_Object *ic = elm_icon_add(obj);

   elm_icon_standard_set(ic, "folder");
   return ic;
}

static void
_dir_item_del(void *data, Evas_Object *obj EINA_UNUSED)
{
   Ephoto_Entry *e = data;

   if (!e->no_delete)
     ephoto_entry_free(e->ephoto, e);
}

static Eina_Bool
_check_for_subdirs(Ephoto_Entry *entry)
{
   Eina_Iterator *ls = eina_file_direct_ls(entry->path);
   Eina_File_Direct_Info *info;

   if (!ls)
     return EINA_FALSE;
   EINA_ITERATOR_FOREACH(ls, info)
     {
        if (info->type == EINA_FILE_DIR)
          {
             eina_iterator_free(ls);
             return EINA_TRUE;
          }
     }
   eina_iterator_free(ls);
   return EINA_FALSE;
}

static void
_trash_back(void *data, Evas_Object *obj EINA_UNUSED,
    void *event_info EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;

   elm_box_clear(db->leftbox);
   db->fsel = db->fsel_back;
   elm_box_pack_end(db->leftbox, db->fsel);
   evas_object_show(db->fsel);
   db->fsel_back = NULL;

   db->thumbs_only = 1;
   db->dirs_only = 0;
   ephoto_directory_set(db->ephoto, db->back_directory, NULL,
       db->dirs_only, db->thumbs_only);
   ephoto_title_set(db->ephoto, db->back_directory);
}

static void
_dir_go_trash(void *data, Evas_Object *obj EINA_UNUSED,
    void *event_info EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;
   char path[PATH_MAX];
   Evas_Object *ic, *but;

   db->fsel_back = db->fsel;
   evas_object_hide(db->fsel_back);
   elm_box_unpack(db->leftbox, db->fsel_back);

   ic = elm_icon_add(db->leftbox);
   evas_object_size_hint_min_set(ic, 20*elm_config_scale_get(),
       20*elm_config_scale_get());
   elm_icon_standard_set(ic, "go-previous");
   evas_object_size_hint_aspect_set(ic, EVAS_ASPECT_CONTROL_BOTH, 1, 1);
   
   but = elm_button_add(db->leftbox);
   elm_object_text_set(but, _("Back"));
   elm_object_part_content_set(but, "icon", ic);
   evas_object_size_hint_weight_set(but, EVAS_HINT_EXPAND, 0.0);
   evas_object_size_hint_fill_set(but, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_smart_callback_add(but, "clicked", _trash_back, db);
   elm_box_pack_end(db->leftbox, but);
   evas_object_show(but);

   db->fsel = elm_genlist_add(db->leftbox);
   elm_genlist_select_mode_set(db->fsel, ELM_OBJECT_SELECT_MODE_NONE);
   elm_genlist_highlight_mode_set(db->fsel, EINA_TRUE);
   evas_object_size_hint_weight_set(db->fsel, EVAS_HINT_EXPAND,
       EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(db->fsel, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_smart_callback_add(db->fsel, "expand,request",
       _on_list_expand_req, db);
   evas_object_smart_callback_add(db->fsel, "contract,request",
       _on_list_contract_req, db);
   evas_object_smart_callback_add(db->fsel, "expanded", _on_list_expanded, db);
   evas_object_smart_callback_add(db->fsel, "contracted", _on_list_contracted,
       db);
   evas_object_event_callback_add(db->fsel, EVAS_CALLBACK_MOUSE_UP,
       _fsel_mouse_up_cb, db);
   evas_object_data_set(db->fsel, "directory_browser", db);
   evas_object_size_hint_min_set(db->fsel, (int)round(195 * elm_config_scale_get()), 0);
   elm_box_pack_end(db->leftbox, db->fsel);
   evas_object_show(db->fsel);

   eina_stringshare_replace(&db->back_directory,
       db->ephoto->config->directory);
   snprintf(path, PATH_MAX, "%s/.config/ephoto/trash", getenv("HOME"));
   if (!ecore_file_exists(path))
      ecore_file_mkpath(path);
   db->thumbs_only = 0;
   db->dirs_only = 0;
   ephoto_directory_set(db->ephoto, path, NULL,
       db->dirs_only, db->thumbs_only);
   ephoto_title_set(db->ephoto, _("Trash"));
   ephoto_directory_browser_top_dir_set(db->ephoto, db->ephoto->config->directory);
}

static Eina_Bool
_click_timer_cb(void *data)
{
   Elm_Object_Item *item = data;
   Ephoto_Directory_Browser *db = evas_object_data_get(item, "directory_browser");

   _on_list_selected(db, NULL, item);
   db->click_timer = NULL;

   return ECORE_CALLBACK_CANCEL;
}

static void
_fsel_menu_new_dir_cb(void *data, Evas_Object *obj EINA_UNUSED,
    void *event_info EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;
   Elm_Object_Item *item = elm_genlist_selected_item_get(db->fsel);
   Ephoto_Entry *entry;
   const char *path;

   if (item)
     {
        entry = elm_object_item_data_get(item);
        path = entry->path;
     }
   else
     path = db->ephoto->config->directory;
   if (!path)
     return;
   ephoto_file_new_dir(db->ephoto, path);
}

static void
_fsel_menu_paste_cb(void *data, Evas_Object *obj EINA_UNUSED,
    void *event_info EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;
   Elm_Object_Item *item = elm_genlist_selected_item_get(db->fsel);

   ephoto_thumb_browser_paste(db->ephoto, item);
}

static void
_fsel_menu_rename_cb(void *data, Evas_Object *obj EINA_UNUSED,
    void *event_info EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;
   Elm_Object_Item *item = elm_genlist_selected_item_get(db->fsel);
   Ephoto_Entry *entry;
   const char *path;

   if (!item)
     return;
   entry = elm_object_item_data_get(item);
   path = entry->path;
   if (!path)
     return;
   ephoto_file_rename(db->ephoto, path);
}

static void
_fsel_menu_delete_cb(void *data, Evas_Object *obj EINA_UNUSED,
    void *event_info EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;
   Elm_Object_Item *item = elm_genlist_selected_item_get(db->fsel);
   Ephoto_Entry *entry;
   Eina_List *files = NULL;
   const char *path;

   if (!item)
     return;
   entry = elm_object_item_data_get(item);
   path = entry->path;
   if (!path)
     return;
   files = eina_list_append(files, path);
   ephoto_file_delete(db->ephoto, files, EINA_FILE_DIR);
}

static void
_fsel_mouse_up_cb(void *data, Evas *e EINA_UNUSED,
    Evas_Object *obj EINA_UNUSED, void *event_info)
{
   Ephoto_Directory_Browser *db = data;
   Evas_Object *menu;
   Elm_Object_Item *item;
   Evas_Event_Mouse_Up *info = event_info;
   char trash[PATH_MAX];
   Evas_Coord x, y;

   evas_pointer_canvas_xy_get(evas_object_evas_get(db->fsel), &x, &y);
   item = elm_genlist_at_xy_item_get(db->fsel, x, y, 0);

   if (info->button == 1 && item)
     {
        if (info->flags == EVAS_BUTTON_DOUBLE_CLICK)
          {
             if (elm_genlist_item_type_get(item) == ELM_GENLIST_ITEM_TREE)
               {
                  if (db->click_timer)
                    {
                       ecore_timer_del(db->click_timer);
                       db->click_timer = NULL;
                       elm_genlist_item_expanded_set(item,
                           !elm_genlist_item_expanded_get(item));
                    }
               }
          }
        else
          {
             evas_object_data_set(item, "directory_browser", db);
             if (elm_genlist_item_type_get(item) == ELM_GENLIST_ITEM_TREE)
               db->click_timer = ecore_timer_add(.3, _click_timer_cb, item);
             else
               _on_list_selected(db, NULL, item);
          }
     }
   else if (!item)
     {
        Elm_Object_Item *it;

        it = elm_genlist_selected_item_get(db->fsel);
        if (it)
          elm_genlist_item_selected_set(it, EINA_FALSE);
        ephoto_directory_set(db->ephoto, db->ephoto->top_directory, NULL, 0, 1);
     }

   if (info->button != 3)
      return;

   snprintf(trash, PATH_MAX, "%s/.config/ephoto/trash", getenv("HOME"));

   if (item)
      elm_genlist_item_selected_set(item, EINA_TRUE);

   db->ephoto->menu_blocking = EINA_TRUE;

   menu = elm_menu_add(db->ephoto->win);
   elm_menu_move(menu, x, y);

   if (strcmp(db->ephoto->config->directory, trash))
     {
        elm_menu_item_add(menu, NULL, "folder-new", _("New Folder"),
              _fsel_menu_new_dir_cb, db);
     }
   if (item)
     {
        evas_object_data_set(item, "directory_browser", db);
        elm_menu_item_add(menu, NULL, "edit", _("Rename"),
            _fsel_menu_rename_cb, item);
	elm_menu_item_add(menu, NULL, "edit-paste", _("Paste"),
	    _fsel_menu_paste_cb, db);
     }
   else if (!strcmp(db->ephoto->config->directory, trash) &&
     elm_genlist_first_item_get(db->fsel))
     {
        elm_menu_item_add(menu, NULL, "edit-delete", _("Empty Trash"),
            _menu_empty_cb, db);
     }
   if (strcmp(db->ephoto->config->directory, trash) && item)
     {
        elm_menu_item_add(menu, NULL, "edit-delete", _("Delete"),
             _fsel_menu_delete_cb, db);
        elm_menu_item_add(menu, NULL, "user-trash", _("Trash"),
            _dir_go_trash, db);
     }
   evas_object_smart_callback_add(menu, "dismissed", _menu_dismissed_cb,
            db);
   evas_object_show(menu);
}

static void
_ephoto_directory_view_add(Ephoto_Directory_Browser *db)
{
   Edje_Message_Int_Set *msg;

   msg = alloca(sizeof(Edje_Message_Int_Set) + (1 * sizeof(int)));
   msg->count = 1;
   msg->val[0] = (int)round(220 * elm_config_scale_get());
   edje_object_message_send(elm_layout_edje_get(db->ephoto->layout),
       EDJE_MESSAGE_INT_SET, 2, msg);

   db->leftbox = elm_box_add(db->main);
   elm_box_horizontal_set(db->leftbox, EINA_FALSE);
   elm_box_homogeneous_set(db->leftbox, EINA_FALSE);
   elm_box_padding_set(db->leftbox, 0, -5);
   evas_object_size_hint_weight_set(db->leftbox, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(db->leftbox,
       EVAS_HINT_FILL, EVAS_HINT_FILL);
   elm_box_pack_end(db->main, db->leftbox);
   evas_object_show(db->leftbox);

   db->fsel = elm_genlist_add(db->leftbox);
   elm_genlist_select_mode_set(db->fsel, ELM_OBJECT_SELECT_MODE_NONE);
   elm_genlist_highlight_mode_set(db->fsel, EINA_TRUE);
   evas_object_size_hint_weight_set(db->fsel, EVAS_HINT_EXPAND,
       EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(db->fsel, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_smart_callback_add(db->fsel, "expand,request",
       _on_list_expand_req, db);
   evas_object_smart_callback_add(db->fsel, "contract,request",
       _on_list_contract_req, db);
   evas_object_smart_callback_add(db->fsel, "expanded", _on_list_expanded, db);
   evas_object_smart_callback_add(db->fsel, "contracted", _on_list_contracted,
       db);
   evas_object_event_callback_add(db->fsel, EVAS_CALLBACK_MOUSE_UP,
       _fsel_mouse_up_cb, db);
   evas_object_data_set(db->fsel, "directory_browser", db);
   evas_object_size_hint_min_set(db->fsel, (int)round(195 * elm_config_scale_get()), 0);
   elm_box_pack_end(db->leftbox, db->fsel);
   evas_object_show(db->fsel);

   elm_drop_item_container_add(db->fsel, ELM_SEL_FORMAT_TARGETS,
       _drop_item_getcb, _drop_enter, db, _drop_leave, db, _drop_pos, db,
       _drop_dropcb, NULL);
}

/*Ephoto Populating Functions*/
static void
_todo_items_free(Ephoto_Directory_Browser *db)
{
   eina_list_free(db->todo_items);
   db->todo_items = NULL;
}

static void
_monitor_cb(void *data, Ecore_File_Monitor *em EINA_UNUSED,
    Ecore_File_Event event, const char *path)
{
   Elm_Object_Item *item;
   Ephoto_Entry *entry = data;
   Ephoto_Entry *e;
   char file[PATH_MAX], dir[PATH_MAX];
   const Elm_Genlist_Item_Class *ic;
   char buf[PATH_MAX];

   if (!entry)
     return;

   snprintf(file, PATH_MAX, "%s", path);
   snprintf(dir, PATH_MAX, "%s", ecore_file_dir_get(file));

   if (strcmp(entry->path, dir))
     return;
   if (event == ECORE_FILE_EVENT_CREATED_DIRECTORY)
     {
        if (!ecore_file_is_dir(path))
          return;
        if (ephoto_entry_exists(entry->ephoto, path))
          return;
 
        if (elm_genlist_item_type_get(entry->item) == ELM_GENLIST_ITEM_TREE &&
            elm_genlist_item_expanded_get(entry->item) == EINA_TRUE)
          {
             ic = &_ephoto_dir_class;
             snprintf(buf, PATH_MAX, "%s", path);
             e = ephoto_entry_new(entry->ephoto, path, basename(buf),
                 EINA_FILE_DIR);
             e->genlist = entry->genlist;
             e->parent = entry->item;
             e->item =
                 elm_genlist_item_sorted_insert(entry->genlist, ic, e,
                 e->parent, ELM_GENLIST_ITEM_NONE, _entry_cmp, NULL, NULL);
             if (e->item)
               e->monitor = ecore_file_monitor_add(e->path, _monitor_cb, e);
          }
        if (elm_genlist_item_type_get(entry->item) == ELM_GENLIST_ITEM_NONE)
          {
             Elm_Object_Item *parent;

             ic = &_ephoto_dir_class;
             parent =
                 elm_genlist_item_insert_before(entry->genlist, ic, entry,
                     entry->parent, entry->item, ELM_GENLIST_ITEM_TREE, NULL, NULL);
             entry->no_delete = EINA_TRUE;
             if (entry->monitor)
               ecore_file_monitor_del(entry->monitor);
             elm_object_item_del(entry->item);
             entry->item = parent;
             entry->no_delete = EINA_FALSE;
             entry->monitor = ecore_file_monitor_add(entry->path, _monitor_cb, entry);
          }
        return;
     }
   else if (event == ECORE_FILE_EVENT_DELETED_DIRECTORY)
     {
        item = elm_genlist_first_item_get(entry->genlist);
        while (item)
          {
             e = elm_object_item_data_get(item);
             if (!strcmp(e->path, path))
               {
                  elm_object_item_del(e->item);
                  //if (!strcmp(e->path, e->ephoto->config->directory))
                  break;
               }
             item = elm_genlist_item_next_get(item);
          }
        if (elm_genlist_item_type_get(entry->item) == ELM_GENLIST_ITEM_TREE &&
            _check_for_subdirs(entry) == EINA_FALSE)
          {
             Elm_Object_Item *parent;

             ic = &_ephoto_dir_class;
             parent =
                 elm_genlist_item_insert_before(entry->genlist, ic, entry,
                 entry->parent, entry->item, ELM_GENLIST_ITEM_NONE, NULL, NULL);
             entry->no_delete = EINA_TRUE;
             elm_object_item_del(entry->item);
             entry->item = parent;
             entry->no_delete = EINA_FALSE;
          }
        if (!ecore_file_exists(entry->ephoto->config->directory))
          {
             ephoto_directory_set(entry->ephoto, entry->path, entry->parent, 0, 1);
             ephoto_title_set(entry->ephoto, entry->path);
          }
        return;
     }
   else if (event == ECORE_FILE_EVENT_MODIFIED)
     {
        if (!ecore_file_is_dir(path))
          return;
        if ((elm_genlist_item_expanded_get(entry->item) == EINA_TRUE))
          {
             item = elm_genlist_first_item_get(entry->genlist);
             while (item)
               {
                  e = elm_object_item_data_get(item);
                  if (!strcmp(e->path, path))
                    {
                       elm_genlist_item_update(e->item);
                       break;
                    }
                  item = elm_genlist_item_next_get(item);
               }
          }
        return;
     }
}

static void
_top_monitor_cb(void *data, Ecore_File_Monitor *em EINA_UNUSED,
    Ecore_File_Event event, const char *path)
{
   Elm_Object_Item *item;
   Ephoto_Directory_Browser *db = data;
   Ephoto_Entry *e;
   const Elm_Genlist_Item_Class *ic;
   char buf[PATH_MAX], file[PATH_MAX], dir[PATH_MAX];

   if (!db)
     return;
   snprintf(file, PATH_MAX, "%s", path);
   snprintf(dir, PATH_MAX, "%s", ecore_file_dir_get(file));

   if (strcmp(db->ephoto->top_directory, dir))
     return;
   if (event == ECORE_FILE_EVENT_CREATED_DIRECTORY)
     {
       if (!ecore_file_is_dir(path))
         return; 
        if (ephoto_entry_exists(db->ephoto, path))
          return;
        snprintf(buf, PATH_MAX, "%s", path);
        e = ephoto_entry_new(db->ephoto, path, basename(buf),
            EINA_FILE_DIR);
        e->genlist = db->fsel;
        ic = &_ephoto_dir_class;
        e->item =
            elm_genlist_item_append(db->fsel, ic, e,
            NULL, ELM_GENLIST_ITEM_NONE, NULL, NULL);
        if (e->item)
          e->monitor = ecore_file_monitor_add(e->path, _monitor_cb, e);
        return;
     }
   else if (event == ECORE_FILE_EVENT_DELETED_DIRECTORY)
     {
        item = elm_genlist_first_item_get(db->fsel);
        while (item)
          {
             e = elm_object_item_data_get(item);
             if (!strcmp(e->path, path))
               {
                  if (!strcmp(path, db->ephoto->config->directory))
                    elm_genlist_item_expanded_set(e->parent, EINA_TRUE);
                  else
                    elm_object_item_del(e->item);
                  break;
               }
             item = elm_genlist_item_next_get(item);
          }
        return;
     }
   else if (event == ECORE_FILE_EVENT_MODIFIED)
     {
        if (!ecore_file_is_dir(path))
          return;
        item = elm_genlist_first_item_get(db->fsel);
        while (item)
          {
             e = elm_object_item_data_get(item);
             if (!strcmp(e->path, path))
               {
                  elm_genlist_item_update(e->item);
                  break;
               }
             item = elm_genlist_item_next_get(item);
          }
        return;
     }
}

static Eina_Bool
_todo_items_process(void *data)
{
   Ephoto_Directory_Browser *db = data;
   Ephoto_Entry *entry;
   int i = 0;

   if ((!db->ls) && (db->animator.processed == db->animator.count))
     {
        if (db->animator.count == 0)
          return EINA_TRUE;
        db->animator.todo_items = NULL;
        db->processing = 0;
	return EINA_FALSE;
     }
   if ((db->ls) && (eina_list_count(db->todo_items) < TODO_ITEM_MIN_BATCH))
      return EINA_TRUE;

   db->animator.todo_items = NULL;
   db->processing = 1;
   EINA_LIST_FREE(db->todo_items, entry)
   {
      i++;
      if (i > TODO_ITEM_MIN_BATCH)
	 return EINA_TRUE;
      if (entry->is_dir && !entry->item)
        {
	   const Elm_Genlist_Item_Class *ic;

	   ic = &_ephoto_dir_class;
	   if (_check_for_subdirs(entry))
             entry->item =
                 elm_genlist_item_sorted_insert(db->fsel, ic, entry,
                 entry->parent, ELM_GENLIST_ITEM_TREE, _entry_cmp, NULL, NULL);
           else
             entry->item =
                 elm_genlist_item_sorted_insert(db->fsel, ic, entry,
                 entry->parent, ELM_GENLIST_ITEM_NONE, _entry_cmp, NULL, NULL);
	   if (!entry->item)
	     {
		ephoto_entry_free(db->ephoto, entry);
	     }
           else
             {
               entry->monitor = ecore_file_monitor_add(entry->path, _monitor_cb, entry);
               entry->genlist = db->fsel;
             }
        }
      db->animator.processed++;
   }
   return EINA_TRUE;
}

static Eina_Bool
_ephoto_dir_populate_start(void *data, int type EINA_UNUSED,
    void *event EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;

   evas_object_smart_callback_call(db->main, "changed,directory", NULL);

   db->animator.processed = 0;
   db->animator.count = 0;
   _todo_items_free(db);
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_ephoto_dir_populate_end(void *data, int type EINA_UNUSED,
    void *event EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;

   db->ls = NULL;
   if (db->main_deleted)
     {
	free(db);
	return ECORE_CALLBACK_PASS_ON;
     }
   db->dirs_only = 0;
   db->thumbs_only = 0;

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_ephoto_dir_populate_error(void *data, int type EINA_UNUSED,
    void *event EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;

   db->dirs_only = 0;
   db->thumbs_only = 0;

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_ephoto_dir_entry_create(void *data, int type EINA_UNUSED, void *event)
{
   Ephoto_Directory_Browser *db = data;
   Ephoto_Event_Entry_Create *ev = event;
   Ephoto_Entry *e;

   e = ev->entry;
   if (e->is_dir)
     {
	db->todo_items = eina_list_append(db->todo_items, e);
	db->animator.count++;
     }
   if (!db->animator.todo_items)
      db->animator.todo_items = ecore_animator_add(_todo_items_process, db);

   return ECORE_CALLBACK_PASS_ON;
}

/*Ephoto Directory Browser Main Callbacks*/
static void
_ephoto_main_del(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED,
    void *event_info EINA_UNUSED)
{
   Ephoto_Directory_Browser *db = data;
   Ecore_Event_Handler *handler;

   _todo_items_free(db);
   EINA_LIST_FREE(db->handlers, handler) ecore_event_handler_del(handler);
   if (db->animator.todo_items)
     {
	ecore_animator_del(db->animator.todo_items);
	db->animator.todo_items = NULL;
     }
   if (db->ls)
     {
	db->main_deleted = EINA_TRUE;
	eio_file_cancel(db->ls);
	return;
     }
   if (db->monitor)
     ecore_file_monitor_del(db->monitor);
   free(db);
}

void
ephoto_directory_browser_top_dir_set(Ephoto *ephoto, const char *dir)
{
   Ephoto_Directory_Browser *db =
       evas_object_data_get(ephoto->dir_browser, "directory_browser");

   if (db->monitor)
     ecore_file_monitor_del(db->monitor);
   if (ephoto->top_directory)
     eina_stringshare_replace(&ephoto->top_directory, dir);
   else
     ephoto->top_directory = eina_stringshare_add(dir);
   db->monitor = ecore_file_monitor_add(dir, _top_monitor_cb, db);
}

void
ephoto_directory_browser_initialize_structure(Ephoto *ephoto)
{
   Ephoto_Directory_Browser *db =
       evas_object_data_get(ephoto->dir_browser, "directory_browser");
   Eina_List *dirs = NULL, *l;
   Elm_Object_Item *next = NULL, *cur = NULL;
   char path[PATH_MAX], *dir;

   db->initializing = EINA_TRUE;
   snprintf(path, PATH_MAX, "%s", ephoto->config->directory);
   dirs = eina_list_prepend(dirs, strdup(path));
   dir = strdup(path);
   while (strcmp(dir, "/"))
     {
        if (dir)
          {
             free(dir);
             dir = NULL;
          }
        dir = ecore_file_dir_get(path);
        dirs = eina_list_prepend(dirs, strdup(dir));
        memset(path, 0x00, sizeof(path));
        snprintf(path, PATH_MAX, "%s", dir);
     }
   if (dir)
     {
        free(dir);
        dir = NULL;
     }
   EINA_LIST_FOREACH(dirs, l, dir)
     {
        Eina_Iterator *it;
        Eina_File_Direct_Info *finfo;
        const char *n = eina_list_data_get(eina_list_next(l));

        it = eina_file_direct_ls(dir);
        cur = next; 
        EINA_ITERATOR_FOREACH(it, finfo)
          {
             if (finfo->type == EINA_FILE_DIR && 
                 strncmp(finfo->path + finfo->name_start, ".", 1))
               {
                  Ephoto_Entry *entry = ephoto_entry_new(db->ephoto, finfo->path,
                      finfo->path+finfo->name_start, finfo->type);
                  entry->parent = cur;
                  if (entry->is_dir && !entry->item)
                    {
                       const Elm_Genlist_Item_Class *ic;
                       ic = &_ephoto_dir_class;
                       if (_check_for_subdirs(entry))
                         entry->item =
                             elm_genlist_item_sorted_insert(db->fsel, ic, entry,
                             entry->parent, ELM_GENLIST_ITEM_TREE, _entry_cmp, NULL, NULL);
                       else
                         entry->item =
                             elm_genlist_item_sorted_insert(db->fsel, ic, entry,
                             entry->parent, ELM_GENLIST_ITEM_NONE, _entry_cmp, NULL, NULL);
                       if (!entry->item)
                         {
                            ephoto_entry_free(db->ephoto, entry);
                         }
                       else
                         {
                            entry->monitor = ecore_file_monitor_add(entry->path, _monitor_cb, entry);
                            entry->genlist = db->fsel;
                         }
                       if (n)
                         {
                            if (!strcmp(n, entry->path))
                              {
                                 next = entry->item;
                                 elm_genlist_item_expanded_set(next, EINA_TRUE);
                              }
                         }
                    }
               }
          }
        free(dir);  
     }
   ephoto_directory_set(db->ephoto, db->ephoto->config->directory, next, EINA_FALSE, EINA_TRUE);
   ephoto_directory_browser_top_dir_set(db->ephoto, "/");
   db->initializing = EINA_FALSE;
}

Evas_Object *
ephoto_directory_browser_add(Ephoto *ephoto, Evas_Object *parent)
{
   Evas_Object *box = elm_box_add(parent);
   Ephoto_Directory_Browser *db;

   EINA_SAFETY_ON_NULL_RETURN_VAL(box, NULL);

   db = calloc(1, sizeof(Ephoto_Directory_Browser));
   EINA_SAFETY_ON_NULL_GOTO(db, error);

   _ephoto_dir_class.item_style = "tree_effect";
   _ephoto_dir_class.func.text_get = _dir_item_text_get;
   _ephoto_dir_class.func.content_get = _dir_item_icon_get;
   _ephoto_dir_class.func.state_get = NULL;
   _ephoto_dir_class.func.del = _dir_item_del;

   db->ephoto = ephoto;
   db->dir_current = NULL;
   db->main = box;

   elm_box_horizontal_set(db->main, EINA_FALSE);
   evas_object_size_hint_weight_set(db->main, EVAS_HINT_EXPAND,
       EVAS_HINT_EXPAND);
   evas_object_size_hint_align_set(db->main, EVAS_HINT_FILL, EVAS_HINT_FILL);
   evas_object_event_callback_add(db->main, EVAS_CALLBACK_DEL,
       _ephoto_main_del, db);
   evas_object_size_hint_min_set(db->main, (int)round(195 * elm_config_scale_get()), 0);
   evas_object_data_set(db->main, "directory_browser", db);

   _ephoto_directory_view_add(db);

   db->handlers =
       eina_list_append(db->handlers,
       ecore_event_handler_add(EPHOTO_EVENT_POPULATE_START,
	   _ephoto_dir_populate_start, db));

   db->handlers =
       eina_list_append(db->handlers,
       ecore_event_handler_add(EPHOTO_EVENT_POPULATE_END,
	   _ephoto_dir_populate_end, db));

   db->handlers =
       eina_list_append(db->handlers,
       ecore_event_handler_add(EPHOTO_EVENT_POPULATE_ERROR,
	   _ephoto_dir_populate_error, db));

   db->handlers =
       eina_list_append(db->handlers,
       ecore_event_handler_add(EPHOTO_EVENT_ENTRY_CREATE,
	   _ephoto_dir_entry_create, db));

   return db->main;

  error:
   evas_object_del(db->main);
   return NULL;
}