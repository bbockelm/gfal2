/* 
* Copyright @ Members of the EMI Collaboration, 2010.
* See www.eu-emi.eu for details on the copyright holders.
* 
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at 
*
*    http://www.apache.org/licenses/LICENSE-2.0 
* 
* Unless required by applicable law or agreed to in writing, software 
* distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/


/*
 * @file gfal_posix_open.c
 * @brief file for the internal open function for the posix interface
 * @author Devresse Adrien
 * @version 2.0
 * @date 31/05/2011
 * */

#include <glib.h>
#include <stdlib.h>


#include <common/gfal_prototypes.h>
#include <common/gfal_types.h>
#include <common/gfal_common_plugin.h>
#include <common/gfal_common_file_handle.h>


#include "gfal_posix_internal.h"


/*
 *  Implementation of gfal_open
 * */
int gfal_posix_internal_open(const char* path, int flag, mode_t mode){
	GError* tmp_err=NULL;
	gfal_handle handle;
	int key = -1;
	
	gfal_log(GFAL_VERBOSE_TRACE, "%s ->",__func__);

	if((handle = gfal_posix_instance()) == NULL){
		errno = EIO;
		return -1;
	}
    key = gfal2_open(handle, path, flag, &tmp_err);
	if(tmp_err){
		gfal_posix_register_internal_error(handle, "[gfal_open]", tmp_err);
		errno = tmp_err->code;	
	}else
		errno=0;
	return key; 	
}
