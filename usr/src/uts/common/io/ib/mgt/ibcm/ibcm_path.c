/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/ib/mgt/ibcm/ibcm_impl.h>
#include <sys/ib/ibtl/ibti.h>

/*
 * ibcm_path.c
 *
 * ibt_get_paths() implement the Path Informations related functionality.
 */

/* Externs. */
extern char	cmlog[];

/* ibcm_saa_service_rec() fills in ServiceID and DGID. */
typedef struct ibcm_dest_s {
	ib_gid_t	d_gid;
	ib_svc_id_t	d_sid;
	ibt_srv_data_t	d_sdata;
	ib_pkey_t	d_pkey;
	uint_t		d_tag;	/* 0 = Unicast, 1 = Multicast, 2 = LoopBack */
} ibcm_dest_t;

/* Holds Destination information needed to fill in ibt_path_info_t. */
typedef struct ibcm_dinfo_s {
	uint8_t		num_dest;
	ib_pkey_t	p_key;
	ibcm_dest_t	dest[1];
} ibcm_dinfo_t;

_NOTE(SCHEME_PROTECTS_DATA("Temporary path storage", ibcm_dinfo_s))
_NOTE(READ_ONLY_DATA(ibt_path_attr_s))

typedef struct ibcm_path_tqargs_s {
	ibt_path_attr_t		attr;
	ibt_path_info_t		*paths;
	uint8_t			*num_paths_p;
	ibt_path_handler_t	func;
	void			*arg;
	ibt_path_flags_t	flags;
	uint8_t			max_paths;
} ibcm_path_tqargs_t;


/* Prototype Declarations. */
static ibt_status_t ibcm_get_path_rec(ibcm_path_tqargs_t *, ibcm_dinfo_t *,
    uint8_t *, ibt_path_info_t *);

static ibt_status_t ibcm_saa_path_rec(ibcm_path_tqargs_t *,
    ibtl_cm_port_list_t *, ibcm_dinfo_t *, uint8_t *);

static ibt_status_t ibcm_update_cep_info(sa_path_record_t *,
    ibtl_cm_port_list_t *, ibtl_cm_hca_port_t *, ibt_cep_path_t *);

static ibt_status_t ibcm_fillin_loopbackinfo(ibtl_cm_port_list_t *,
    uint8_t index, ibcm_dinfo_t *, ibt_path_info_t *);

static ibt_status_t ibcm_saa_service_rec(ibcm_path_tqargs_t *,
    ibtl_cm_port_list_t *, ibcm_dinfo_t *);

static ibt_status_t ibcm_get_single_pathrec(ibcm_path_tqargs_t *,
    ibtl_cm_port_list_t *, ibcm_dinfo_t *, uint8_t,
    uint8_t *, ibt_path_info_t *);

static ibt_status_t ibcm_get_multi_pathrec(ibcm_path_tqargs_t *,
    ibtl_cm_port_list_t *, ibcm_dinfo_t *dinfo,
    uint8_t *, ibt_path_info_t *);

static ibt_status_t ibcm_validate_path_attributes(ibt_path_attr_t *attrp,
    ibt_path_flags_t flags, uint8_t max_paths);

static ibt_status_t ibcm_handle_get_path(ibt_path_attr_t *attrp,
    ibt_path_flags_t flags, uint8_t max_paths, ibt_path_info_t *paths,
    uint8_t *num_path_p, ibt_path_handler_t func, void  *arg);

static void ibcm_process_async_get_paths(void *tq_arg);

static ibt_status_t ibcm_process_get_paths(void *tq_arg);

static ibt_status_t ibcm_get_comp_pgids(ib_gid_t, ib_gid_t, ib_guid_t,
    ib_gid_t **, uint_t *);


/*
 * Function:
 *	ibt_aget_paths
 * Input:
 *	ibt_hdl		The handle returned to the client by the IBTF from an
 *			ibt_attach() call. Can be used by the IBTF Policy module
 *			and CM in the determination of the "best" path to the
 *			specified destination for this class of driver.
 *	flags		Path flags.
 *	attrp		Points to an ibt_path_attr_t struct that contains
 *			required and optional attributes.
 *	func		A pointer to an ibt_path_handler_t function to call
 *			when ibt_aget_paths() completes.
 *	arg		The argument to 'func'.
 * Returns:
 *	IBT_SUCCESS on early validation of attributes else appropriate error.
 * Description:
 *	Finds the best path to a specified destination or service
 *	asynchronously (as determined by the IBTL) that satisfies the
 *	requirements specified in an ibt_path_attr_t struct.
 *	ibt_aget_paths() is a Non-Blocking version of ibt_get_paths().
 */
ibt_status_t
ibt_aget_paths(ibt_clnt_hdl_t ibt_hdl, ibt_path_flags_t flags,
    ibt_path_attr_t *attrp, uint8_t max_paths, ibt_path_handler_t func,
    void  *arg)
{
	IBTF_DPRINTF_L3(cmlog, "ibt_aget_paths(%p, 0x%lX, %p, %d, %p)",
	    ibt_hdl, flags, attrp, max_paths, func);

	if (func == NULL) {
		IBTF_DPRINTF_L2(cmlog, "ibt_aget_paths: Function Pointer is "
		    "NULL - ERROR ");
		return (IBT_INVALID_PARAM);
	}

	/* Memory for path info will be allocated in ibcm_process_get_paths() */
	return (ibcm_handle_get_path(attrp, flags, max_paths, NULL, NULL,
	    func, arg));
}


/*
 * Function:
 *	ibt_get_paths
 * Input:
 *	ibt_hdl		The handle returned to the client by the IBTF from an
 *			ibt_attach() call. Can be used by the IBTF Policy module
 *			and CM in the determination of the "best" path to the
 *			specified destination for this class of driver.
 *	flags		Path flags.
 *	attrp		Points to an ibt_path_attr_t struct that contains
 *			required and optional attributes.
 *	max_paths	The size of the "paths" array argument. Also, this
 *			is the limit on the number of paths returned.
 *			max_paths indicates the number of requested paths to
 *			the specified destination(s).
 * Output:
 *	paths		An array of ibt_path_info_t structs filled in by
 *			ibt_get_paths() as output parameters. Upon return,
 *			array elements with non-NULL HCA GUIDs are valid.
 *	num_paths_p	If non-NULL, return the actual number of paths found.
 * Returns:
 *	IBT_SUCCESS on Success else appropriate error.
 * Description:
 *	Finds the best path to a specified destination (as determined by the
 *	IBTL) that satisfies the requirements specified in an ibt_path_attr_t
 *	struct.
 *
 *	This routine can not be called from interrupt context.
 */
ibt_status_t
ibt_get_paths(ibt_clnt_hdl_t ibt_hdl, ibt_path_flags_t flags,
    ibt_path_attr_t *attrp, uint8_t max_paths, ibt_path_info_t *paths,
    uint8_t *num_paths_p)
{
	ASSERT(paths != NULL);

	IBTF_DPRINTF_L3(cmlog, "ibt_get_paths(%p, 0x%lX, %p, %d)",
	    ibt_hdl, flags, attrp, max_paths);

	if (paths == NULL) {
		IBTF_DPRINTF_L2(cmlog, "ibt_get_paths: Path Info Pointer is "
		    "NULL - ERROR ");
		return (IBT_INVALID_PARAM);
	}

	if (num_paths_p != NULL)
		*num_paths_p = 0;

	return (ibcm_handle_get_path(attrp, flags, max_paths, paths,
	    num_paths_p, NULL, NULL));
}


static ibt_status_t
ibcm_handle_get_path(ibt_path_attr_t *attrp, ibt_path_flags_t flags,
    uint8_t max_paths, ibt_path_info_t *paths, uint8_t *num_path_p,
    ibt_path_handler_t func, void  *arg)
{
	ibcm_path_tqargs_t	*path_tq;
	int		sleep_flag = ((func == NULL) ? KM_SLEEP : KM_NOSLEEP);
	int		len;
	ibt_status_t	retval;

	retval = ibcm_validate_path_attributes(attrp, flags, max_paths);
	if (retval != IBT_SUCCESS)
		return (retval);

	len = (attrp->pa_num_dgids * sizeof (ib_gid_t)) +
	    sizeof (ibcm_path_tqargs_t);

	path_tq = kmem_alloc(len, sleep_flag);
	if (path_tq == NULL) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_handle_get_path: "
		    "Unable to allocate memory for local usage.");
		return (IBT_INSUFF_KERNEL_RESOURCE);
	}

	_NOTE(NOW_INVISIBLE_TO_OTHER_THREADS(*path_tq))

	bcopy(attrp, &path_tq->attr, sizeof (ibt_path_attr_t));

	if (attrp->pa_num_dgids) {
		path_tq->attr.pa_dgids = (ib_gid_t *)(((uchar_t *)path_tq) +
		    sizeof (ibcm_path_tqargs_t));

		bcopy(attrp->pa_dgids, path_tq->attr.pa_dgids,
		    sizeof (ib_gid_t) * attrp->pa_num_dgids);
	} else {
		path_tq->attr.pa_dgids = NULL;
	}

	/* Ignore IBT_PATH_AVAIL flag, if only one path is requested. */
	if ((flags & IBT_PATH_AVAIL) && (max_paths == 1)) {
		flags &= ~IBT_PATH_AVAIL;

		IBTF_DPRINTF_L4(cmlog, "ibcm_handle_get_path: "
		    "Ignoring IBT_PATH_AVAIL flag, as only ONE path "
		    "information is requested.");
	}

	path_tq->flags = flags;
	path_tq->max_paths = max_paths;
	path_tq->paths = paths;
	path_tq->num_paths_p = num_path_p;
	path_tq->func = func;
	path_tq->arg = arg;

	_NOTE(NOW_VISIBLE_TO_OTHER_THREADS(*path_tq))

	if (func != NULL) {		/* Non-Blocking */
		IBTF_DPRINTF_L3(cmlog, "ibcm_handle_get_path: Non Blocking");
		if (taskq_dispatch(ibcm_taskq, ibcm_process_async_get_paths,
		    path_tq, TQ_NOSLEEP) == 0) {
			IBTF_DPRINTF_L2(cmlog, "ibcm_handle_get_path: "
			    "Failed to dispatch the TaskQ");
			kmem_free(path_tq, len);
			return (IBT_INSUFF_KERNEL_RESOURCE);
		} else
			return (IBT_SUCCESS);
	} else {		/* Blocking */
		IBTF_DPRINTF_L3(cmlog, "ibcm_handle_get_path: Blocking");
		return (ibcm_process_get_paths(path_tq));
	}
}


static void
ibcm_process_async_get_paths(void *tq_arg)
{
	(void) ibcm_process_get_paths(tq_arg);
}


static ibt_status_t
ibcm_validate_path_attributes(ibt_path_attr_t *attrp, ibt_path_flags_t flags,
    uint8_t max_paths)
{
	uint_t			i;

	IBTF_DPRINTF_L2(cmlog, "ibcm_validate_path_attributes: Inputs are: "
	    "HCA (%llX, %d),\n\tSGID(%llX:%llX), SName=\"%s\",\n\tSID= %llX, "
	    "Maxpath= %d, Flags= 0x%X, #Dgid= %d, SDFlag= 0x%llX",
	    attrp->pa_hca_guid, attrp->pa_hca_port_num,
	    attrp->pa_sgid.gid_prefix, attrp->pa_sgid.gid_guid,
	    ((attrp->pa_sname != NULL) ? attrp->pa_sname : ""), attrp->pa_sid,
	    max_paths, flags, attrp->pa_num_dgids, attrp->pa_sd_flags);

	/*
	 * Validate Path Flags.
	 * IBT_PATH_AVAIL & IBT_PATH_PERF are mutually exclusive.
	 */
	if ((flags & IBT_PATH_AVAIL) && (flags & IBT_PATH_PERF)) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_validate_path_attributes: "
		    "Invalid Flags: 0x%X,\n\t AVAIL and PERF flags cannot "
		    "specified together.", flags);
		return (IBT_INVALID_PARAM);
	}

	/* Validate number of records requested. */
	if ((flags & (IBT_PATH_AVAIL | IBT_PATH_PERF)) &&
	    (max_paths > IBT_MAX_SPECIAL_PATHS)) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_validate_path_attributes: "
		    "Max records that can be requested is <%d> \n"
		    "when IBT_PATH_AVAIL or IBT_PATH_PERF flag is specified.",
		    IBT_MAX_SPECIAL_PATHS);
		return (IBT_INVALID_PARAM);
	}

	/* Only 2 destinations can be specified w/ APM flag. */
	if ((flags & IBT_PATH_APM) && (attrp->pa_num_dgids > 2)) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_validate_path_attributes:\n\t Max "
		    "number of DGIDs that can be specified w/APM flag is 2");
		return (IBT_INVALID_PARAM);
	}

	/*
	 * Max_paths of "0" is invalid.
	 * w/ IBT_PATH_MULTI_SVC_DEST flag, max_paths must be greater than "1".
	 */
	if ((max_paths == 0) ||
	    ((flags & IBT_PATH_MULTI_SVC_DEST) && (max_paths < 2))) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_validate_path_attributes: "
		    "Invalid number of records requested:\n flags 0x%X, "
		    "max_paths %d", flags, max_paths);
		return (IBT_INVALID_PARAM);
	}

	/*
	 * If IBT_PATH_MULTI_SVC_DEST is set, then ServiceName and/or Service ID
	 * must be specified and DGIDs SHOULD NOT be specified.
	 */
	if ((flags & IBT_PATH_MULTI_SVC_DEST) && ((attrp->pa_num_dgids > 0) ||
	    ((attrp->pa_sid == 0) && ((attrp->pa_sname == NULL) ||
	    ((attrp->pa_sname != NULL) && (strlen(attrp->pa_sname) == 0)))))) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_validate_path_attributes: "
		    "Invalid Flags: 0x%X, IBT_PATH_MULTI_SVC_DEST flag set "
		    "but Service Name \n or Service ID NOT specified or DGIDs "
		    "are specified.", flags);
		return (IBT_INVALID_PARAM);
	}

	/*
	 * User need to specify the destination information, which can be
	 * provided as one or more of the following.
	 *	o ServiceName
	 *	o ServiceID
	 *	o Array of DGIDs w/Num of DGIDs, (max of 2)
	 */
	if ((attrp->pa_sid == 0) && (attrp->pa_num_dgids == 0) &&
	    ((attrp->pa_sname == NULL) || ((attrp->pa_sname != NULL) &&
	    (strlen(attrp->pa_sname) == 0)))) {
		/* Destination information not provided, bail out. */
		IBTF_DPRINTF_L2(cmlog, "ibcm_validate_path_attributes: "
		    "Client's MUST supply DestInfo.");
		return (IBT_INVALID_PARAM);
	}

	/* If DGIDs are provided, validate them. */
	if (attrp->pa_num_dgids > 0) {
		if (attrp->pa_dgids == NULL) {
			IBTF_DPRINTF_L2(cmlog, "ibcm_validate_path_attributes: "
			    "pa_dgids NULL, but pa_num_dgids : %d",
			    attrp->pa_num_dgids);
			return (IBT_INVALID_PARAM);
		}

		/* Validate DGIDs */
		for (i = 0; i < attrp->pa_num_dgids; i++) {
			ib_gid_t	gid = attrp->pa_dgids[i];

			IBTF_DPRINTF_L3(cmlog, "ibcm_validate_path_attributes: "
			    "DGID[%d] = %llX:%llX", i, gid.gid_prefix,
			    gid.gid_guid);

			/* APM request for MultiCast destination is invalid. */
			if ((gid.gid_prefix >> 56ULL & 0xFF) == 0xFF) {
				if (flags & IBT_PATH_APM) {
					IBTF_DPRINTF_L2(cmlog,
					    "ibcm_validate_path_attributes: "
					    "APM for MGIDs not supported.");
					return (IBT_INVALID_PARAM);
				}
			} else if ((gid.gid_prefix == 0) ||
			    (gid.gid_guid == 0)) {
				IBTF_DPRINTF_L2(cmlog,
				    "ibcm_validate_path_attributes: ERROR: "
				    "Invalid DGIDs specified");
				return (IBT_INVALID_PARAM);
			}
		}
	}

	/* Check for valid Service Name length. */
	if ((attrp->pa_sname != NULL) &&
	    (strlen(attrp->pa_sname) >= IB_SVC_NAME_LEN)) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_validate_path_attributes: "
		    "ServiceName too long");
		return (IBT_INVALID_PARAM);
	}

	/* If P_Key is specified, check for invalid p_key's */
	if (flags & IBT_PATH_PKEY) {
		/* Limited P_Key is NOT supported as of now!. */
		if ((attrp->pa_pkey == IB_PKEY_INVALID_FULL) ||
		    (attrp->pa_pkey & 0x8000) == 0) {
			IBTF_DPRINTF_L2(cmlog, "ibcm_validate_path_attributes: "
			    "Specified P_Key is invalid: 0x%X", attrp->pa_pkey);
			return (IBT_INVALID_PARAM);
		}
		IBTF_DPRINTF_L3(cmlog, "ibcm_validate_path_attributes: "
		    "P_Key= 0x%X", attrp->pa_pkey);
	}

	return (IBT_SUCCESS);
}


static ibt_status_t
ibcm_process_get_paths(void *tq_arg)
{
	ibcm_path_tqargs_t	*p_arg = (ibcm_path_tqargs_t *)tq_arg;
	ibcm_dinfo_t		*dinfo;
	int			len;
	uint8_t			max_paths, num_path;
	ibt_status_t		retval;
	ib_gid_t		*d_gids_p = NULL;
	ibtl_cm_port_list_t	*slistp = NULL;
	uint_t			dnum = 0, num_dest;
	uint_t			i, j;
	ibcm_hca_info_t		*hcap;
	ibmf_saa_handle_t	saa_handle;

	IBTF_DPRINTF_L3(cmlog, "ibcm_process_get_paths(%p, 0x%X, %d) ",
	    p_arg, p_arg->flags, p_arg->max_paths);

	max_paths = num_path = p_arg->max_paths;

	/*
	 * Prepare the Destination list based on the input DGIDs and
	 * other attributes.
	 *
	 * APM is requested and pa_dgids are specified.  If multiple DGIDs are
	 * specified, check out whether they are companion to each other or if
	 * only one DGID is specified, then get the companion port GID for that.
	 */
	if (p_arg->attr.pa_num_dgids) {
		if (p_arg->flags & IBT_PATH_APM) {
			ib_gid_t	c_gid, n_gid;

			IBTF_DPRINTF_L3(cmlog, "ibcm_process_get_paths: "
			    "DGIDs specified w/ APM Flag");

			c_gid = p_arg->attr.pa_dgids[0];
			if (p_arg->attr.pa_num_dgids > 1)
				n_gid = p_arg->attr.pa_dgids[1];
			else
				n_gid.gid_prefix = n_gid.gid_guid = 0;

			retval = ibcm_get_comp_pgids(c_gid, n_gid, 0, &d_gids_p,
			    &dnum);
			if ((retval != IBT_SUCCESS) &&
			    (retval != IBT_GIDS_NOT_FOUND)) {
				IBTF_DPRINTF_L2(cmlog, "ibcm_process_get_paths:"
				    " Invalid DGIDs specified w/ APM Flag");
				    goto path_error2;
			}
			IBTF_DPRINTF_L3(cmlog, "ibcm_process_get_paths: "
			    "Found %d Comp DGID", dnum);
		}

		if (dnum) {
			len = 1;
		} else {
			len = p_arg->attr.pa_num_dgids - 1;
		}
		num_dest = len + 1;

		IBTF_DPRINTF_L3(cmlog, "ibcm_process_get_paths: #dgid %d, dnum "
		    "%d, #dest %d", p_arg->attr.pa_num_dgids, dnum, num_dest);
	} else {
		if (p_arg->flags & IBT_PATH_MULTI_SVC_DEST) {
			IBTF_DPRINTF_L4(cmlog, "ibcm_process_get_paths: "
			    "IBT_PATH_MULTI_SVC_DEST flags set");
			len = max_paths - 1;
		} else if (p_arg->flags & IBT_PATH_APM) {
			len = 1;
		} else {
			len = 0;
		}
		num_dest = 0;
	}

	/* Allocate memory and accumulate all destination information */
	len = (len * sizeof (ibcm_dest_t)) + sizeof (ibcm_dinfo_t);

	dinfo = kmem_zalloc(len, KM_SLEEP);
	dinfo->num_dest = num_dest;
	if (p_arg->flags & IBT_PATH_PKEY)
		dinfo->p_key = p_arg->attr.pa_pkey;

	for (i = 0, j = 0; i < num_dest; i++) {
		if (i < p_arg->attr.pa_num_dgids)
			dinfo->dest[i].d_gid = p_arg->attr.pa_dgids[i];
		else
			dinfo->dest[i].d_gid = d_gids_p[j++];
	}

	_NOTE(NOW_INVISIBLE_TO_OTHER_THREADS(*p_arg))

	/* IBTF allocates memory for path_info in case of Async Get Paths */
	if (p_arg->paths == NULL)
		p_arg->paths = kmem_zalloc(sizeof (ibt_path_info_t) * max_paths,
		    KM_SLEEP);

	_NOTE(NOW_VISIBLE_TO_OTHER_THREADS(*p_arg))

	/* Checkout whether user has specified SGID. */
	if (p_arg->attr.pa_sgid.gid_prefix && p_arg->attr.pa_sgid.gid_guid) {
		ibtl_cm_hca_port_t	hport;

		IBTF_DPRINTF_L3(cmlog, "ibcm_process_get_paths: SGID %llX:%llX",
		    p_arg->attr.pa_sgid.gid_prefix,
		    p_arg->attr.pa_sgid.gid_guid);

		/* For the specified SGID, get HCA information. */
		retval = ibtl_cm_get_hca_port(p_arg->attr.pa_sgid,
		    p_arg->attr.pa_hca_guid, &hport);
		if (retval != IBT_SUCCESS) {
			IBTF_DPRINTF_L2(cmlog, "ibcm_process_get_paths: "
			    "Get HCA Port Failed: %d", retval);
			goto path_error;
		}

		if ((p_arg->attr.pa_hca_port_num != 0) &&
		    (p_arg->attr.pa_hca_port_num != hport.hp_port)) {
			IBTF_DPRINTF_L2(cmlog, "ibcm_process_get_paths: "
			    "Mis-match input HCA PortNum v/s SGID");
			retval = IBT_HCA_PORT_INVALID;
			goto path_error;
		}

		/*
		 * If a specific MTU is desired, then first check out whether
		 * this source port is capable of this MTU.
		 */
		if (p_arg->attr.pa_mtu.r_mtu) {
			if ((p_arg->attr.pa_mtu.r_selector == IBT_GT) &&
			    (p_arg->attr.pa_mtu.r_mtu >= hport.hp_mtu)) {

				IBTF_DPRINTF_L2(cmlog, "ibcm_process_get_paths:"
				    " Required MTU not available on this Port. "
				    "Requested IBT_GT 0x%x but avail 0x%x",
				    p_arg->attr.pa_mtu.r_mtu, hport.hp_mtu);

				retval = IBT_INVALID_PARAM;
				goto path_error;
			} else if ((p_arg->attr.pa_mtu.r_selector == IBT_EQU) &&
			    (p_arg->attr.pa_mtu.r_mtu > hport.hp_mtu)) {
				IBTF_DPRINTF_L2(cmlog, "ibcm_process_get_paths:"
				    " Required MTU not available on this Port "
				    "Requested IBT_EQU to 0x%x but avail 0x%x",
				    p_arg->attr.pa_mtu.r_mtu, hport.hp_mtu);

				retval = IBT_INVALID_PARAM;
				goto path_error;
			}
		}

		if (p_arg->flags & IBT_PATH_APM) {
			_NOTE(NOW_INVISIBLE_TO_OTHER_THREADS(*p_arg))
			p_arg->attr.pa_hca_guid = hport.hp_hca_guid;
			_NOTE(NOW_VISIBLE_TO_OTHER_THREADS(*p_arg))

			retval = ibtl_cm_get_active_plist(&p_arg->attr,
			    p_arg->flags, &slistp);
			if (retval != IBT_SUCCESS) {
				IBTF_DPRINTF_L2(cmlog, "ibcm_process_get_paths:"
				    " ibtl_cm_get_active_plist returned error "
				    " %d", retval);
				goto path_error;
			}
		} else {
			slistp = kmem_zalloc(sizeof (ibtl_cm_port_list_t),
			    KM_SLEEP);

			_NOTE(NOW_INVISIBLE_TO_OTHER_THREADS(*slistp))
			slistp->p_hca_guid = hport.hp_hca_guid;
			slistp->p_mtu = hport.hp_mtu;
			slistp->p_port_num = hport.hp_port;
			slistp->p_base_lid = hport.hp_base_lid;
			slistp->p_sgid = p_arg->attr.pa_sgid;
			slistp->p_sgid_ix = hport.hp_sgid_ix;
			slistp->p_count = 1;
			_NOTE(NOW_VISIBLE_TO_OTHER_THREADS(*slistp))
		}
	} else {
		/* Source GID is not specified, but so let's find them. */
		if (p_arg->attr.pa_hca_guid)
			IBTF_DPRINTF_L3(cmlog, "ibcm_process_get_paths: Get "
			    "Paths from HCA (%llX)", p_arg->attr.pa_hca_guid);
		else
			IBTF_DPRINTF_L3(cmlog, "ibcm_process_get_paths: SRC "
			    "Point not specified, flags(%X)", p_arg->flags);
		/*
		 * Get list of active HCA<->Port list, that matches input
		 * specified attr.
		 */
		retval = ibtl_cm_get_active_plist(&p_arg->attr, p_arg->flags,
		    &slistp);
		if (retval != IBT_SUCCESS) {
			IBTF_DPRINTF_L2(cmlog, "ibcm_process_get_paths: "
			    "HCA capable of requested source attributes NOT "
			    "available.");
			goto path_error;
		}
	}

	IBTF_DPRINTF_L3(cmlog, "ibcm_process_get_paths: HCA (%llX, %d)",
	    slistp->p_hca_guid, slistp->p_port_num);

	hcap = ibcm_find_hca_entry(slistp->p_hca_guid);
	if (hcap == NULL) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_process_get_paths: "
		    "NO HCA found");
		retval = IBT_HCA_BUSY_DETACHING;
		goto path_error;
	}

	/* Get SA Access Handle. */
	for (i = 0; i < slistp->p_count; i++) {
		if (i == 0) {
			/* Validate whether this HCA supports APM */
			if ((p_arg->flags & IBT_PATH_APM) &&
			    (!(hcap->hca_caps & IBT_HCA_AUTO_PATH_MIG))) {
				IBTF_DPRINTF_L2(cmlog, "ibcm_process_get_paths:"
				    " HCA (%llX): APM NOT SUPPORTED ",
				    slistp[i].p_hca_guid);
				retval = IBT_APM_NOT_SUPPORTED;
				goto path_error1;
			}
		}

		saa_handle = ibcm_get_saa_handle(hcap, slistp[i].p_port_num);
		if (saa_handle == NULL) {
			IBTF_DPRINTF_L2(cmlog, "ibcm_process_get_paths: "
			    "SAA HDL NULL, HCA (%llX:%d) NOT ACTIVE",
			    slistp[i].p_hca_guid, slistp[i].p_port_num);
			retval = IBT_HCA_PORT_NOT_ACTIVE;
			goto path_error1;
		}
		_NOTE(NOW_INVISIBLE_TO_OTHER_THREADS(*slistp))
		slistp[i].p_saa_hdl = saa_handle;
		_NOTE(NOW_VISIBLE_TO_OTHER_THREADS(*slistp))
	}

	/*
	 * If Service Name or Service ID are specified, first retrieve
	 * Service Records.
	 */
	if ((p_arg->attr.pa_sid != 0) || ((p_arg->attr.pa_sname != NULL) &&
	    (strlen(p_arg->attr.pa_sname) != 0))) {

		IBTF_DPRINTF_L3(cmlog, "ibcm_process_get_paths: Get Service "
		    "Record for \n\t(%llX, \"%s\")", p_arg->attr.pa_sid,
		    ((p_arg->attr.pa_sname != NULL) ?
		    p_arg->attr.pa_sname : ""));

		/* Get Service Records. */
		retval = ibcm_saa_service_rec(p_arg, slistp, dinfo);
		if ((retval != IBT_SUCCESS) && (retval != IBT_INSUFF_DATA)) {
			IBTF_DPRINTF_L2(cmlog, "ibcm_process_get_paths: Status="
			    "%d, Failed to get Service Record for \n\t"
			    "(%llX, \"%s\")", retval, p_arg->attr.pa_sid,
			    ((p_arg->attr.pa_sname != NULL) ?
			    p_arg->attr.pa_sname : ""));
			goto path_error1;
		}
	}

	/* Get Path Records. */
	retval = ibcm_saa_path_rec(p_arg, slistp, dinfo, &num_path);

path_error1:
	ibcm_dec_hca_acc_cnt(hcap);

path_error:
	if (slistp)
		ibtl_cm_free_active_plist(slistp);

	if (dinfo)
		kmem_free(dinfo, len);

path_error2:
	if ((retval != IBT_SUCCESS) && (retval != IBT_INSUFF_DATA))
		num_path = 0;

	if (p_arg->num_paths_p != NULL)
		*p_arg->num_paths_p = num_path;

	if ((dnum) && (d_gids_p))
		kmem_free(d_gids_p, dnum * sizeof (ib_gid_t));

	if (p_arg->func) {   /* Do these only for Async Get Paths */
		ibt_path_info_t *tmp_path_p;

		if (retval == IBT_INSUFF_DATA) {
			/*
			 * We allocated earlier memory based on "max_paths",
			 * but we got lesser path-records, so re-adjust that
			 * buffer so that caller can free the correct memory.
			 */
			tmp_path_p = kmem_alloc(
			    sizeof (ibt_path_info_t) * num_path, KM_SLEEP);

			bcopy(p_arg->paths, tmp_path_p,
			    num_path * sizeof (ibt_path_info_t));

			kmem_free(p_arg->paths,
			    sizeof (ibt_path_info_t) * max_paths);
		} else if (retval != IBT_SUCCESS) {
			if (p_arg->paths)
				kmem_free(p_arg->paths,
				    sizeof (ibt_path_info_t) * max_paths);
			tmp_path_p = NULL;
		} else {
			tmp_path_p = p_arg->paths;
		}
		(*(p_arg->func))(p_arg->arg, retval, tmp_path_p, num_path);
	}

	len = (sizeof (ib_gid_t) * p_arg->attr.pa_num_dgids) +
	    sizeof (ibcm_path_tqargs_t);

	if (p_arg && len)
		kmem_free(p_arg, len);

	IBTF_DPRINTF_L2(cmlog, "ibcm_process_get_paths: done: status %d, "
	    "Found %d/%d Path Records", retval, num_path, max_paths);

	return (retval);
}


/*
 * Perform SA Access to retrieve Path Records.
 */
static ibt_status_t
ibcm_saa_path_rec(ibcm_path_tqargs_t *p_arg, ibtl_cm_port_list_t *sl,
    ibcm_dinfo_t *dinfo, uint8_t *max_count)
{
	uint8_t		num_path = *max_count;
	uint8_t		num_path_plus;
	uint_t		extra, idx, rec_found = 0;
	ibt_status_t	retval = IBT_SUCCESS;
	int		unicast_dgid_present = 0;
	uint8_t		i;

	IBTF_DPRINTF_L3(cmlog, "ibcm_saa_path_rec(%p, %p, %p, 0x%X, %d)",
	    p_arg, sl, dinfo, p_arg->flags, *max_count);

	if ((dinfo->num_dest == 0) || (num_path == 0) || (sl == NULL)) {
		IBTF_DPRINTF_L3(cmlog, "ibcm_saa_path_rec: Invalid Counters");
		return (IBT_INVALID_PARAM);
	}

	/*
	 * Of the total needed "X" number of paths to "Y" number of destination
	 * we need to get X/Y plus X%Y extra paths to each destination,
	 * We do this so that we can choose the required number of path records
	 * for the specific destination.
	 */
	num_path /= dinfo->num_dest;
	extra = (*max_count % dinfo->num_dest);

	IBTF_DPRINTF_L3(cmlog, "ibcm_saa_path_rec: numpath %d extra %d dest %d",
	    num_path, extra, dinfo->num_dest);

	/*
	 * Find out whether we need to get PathRecord for a MGID as DGID or
	 * qualifies for a LoopBack.
	 */
	for (idx = 0; idx < dinfo->num_dest; idx++) {
		ib_gid_t	dgid = dinfo->dest[idx].d_gid;

		IBTF_DPRINTF_L3(cmlog, "ibcm_saa_path_rec: DGID[%d]: %llX:%llX",
		    idx, dgid.gid_prefix, dgid.gid_guid);

		if ((dgid.gid_prefix >> 56ULL & 0xFF) == 0xFF) {
			if (extra)
				num_path_plus = num_path + 1;
			else
				num_path_plus = num_path;

			IBTF_DPRINTF_L3(cmlog, "ibcm_saa_path_rec: Get %d Paths"
			    "- MGID(%016llX%016llX)", num_path_plus,
			    dgid.gid_prefix, dgid.gid_guid);

			dinfo->dest[idx].d_tag = 1; /* MultiCast */

			/* Yes, it's Single PathRec query for MGID as DGID. */
			retval = ibcm_get_single_pathrec(p_arg, sl, dinfo, idx,
			    &num_path_plus, &p_arg->paths[rec_found]);
			if ((retval != IBT_SUCCESS) &&
			    (retval != IBT_INSUFF_DATA)) {
				IBTF_DPRINTF_L3(cmlog, "ibcm_saa_path_rec: "
				    "Failed to get PathRec for MGID %d",
				    retval);
				continue;
			}
			if (extra)
				extra--;

			rec_found += num_path_plus;
		} else {
			/*
			 * Check out whether we are looking for loop-back path
			 * info. In this case, we should not contact SA Access
			 * for Path Records, but instead we need to "synthesize"
			 * a loop back path record.
			 */
			for (i = 0; i < sl->p_count; i++) {
				if ((sl[i].p_sgid.gid_prefix ==
				    dgid.gid_prefix) &&
				    (sl[i].p_sgid.gid_guid == dgid.gid_guid)) {

					dinfo->dest[idx].d_tag = 2;

					/* Yes, it's loop back case. */
					retval = ibcm_fillin_loopbackinfo(
					    &sl[i], idx, dinfo,
					    &p_arg->paths[rec_found]);
					if (retval != IBT_SUCCESS)
						break;

					/*
					 * We update only one record for
					 * loop-back case.
					 */
					rec_found++;
					if (rec_found == *max_count)
						break;
				}
			}
		}
		if (rec_found == *max_count)
			break;
	}

	for (i = 0; i < dinfo->num_dest; i++) {
		if (dinfo->dest[i].d_tag == 0) {
			unicast_dgid_present++;
		}
	}

	num_path_plus = *max_count - rec_found;

	IBTF_DPRINTF_L3(cmlog, "ibcm_saa_path_rec: Recfound: %d, need to find "
	    "%d, UniCastGID present %d", rec_found, num_path_plus,
	    unicast_dgid_present);

	if ((unicast_dgid_present != 0) && (num_path_plus > 0)) {
		IBTF_DPRINTF_L3(cmlog, "ibcm_saa_path_rec: MultiSM=%X, #SRC=%d,"
		    "Dest%d", sl->p_multi, sl->p_count, unicast_dgid_present);

		if ((sl->p_multi != IBTL_CM_SIMPLE_SETUP) ||
		    ((unicast_dgid_present == 1) && (sl->p_count == 1))) {
			/*
			 * Use SinglePathRec if we are dealing w/ MultiSM or
			 * request is for one SGID to one DGID.
			 */
			retval = ibcm_get_single_pathrec(p_arg, sl, dinfo, 0xFF,
			    &num_path_plus, &p_arg->paths[rec_found]);
		} else {
			/* MultiPathRec will be used for other queries. */
			retval = ibcm_get_multi_pathrec(p_arg, sl, dinfo,
			    &num_path_plus, &p_arg->paths[rec_found]);
		}
		if ((retval != IBT_SUCCESS) && (retval != IBT_INSUFF_DATA)) {
			IBTF_DPRINTF_L2(cmlog, "ibcm_saa_path_rec: "
			    "Failed to get PathRec: Status %d", retval);
		} else {
			rec_found += num_path_plus;
		}
	}

	if ((rec_found == 0) && (retval == IBT_SUCCESS))
		retval = IBT_PATH_RECORDS_NOT_FOUND;
	else if (rec_found != *max_count)
		retval = IBT_INSUFF_DATA;
	else if (rec_found != 0)
		retval = IBT_SUCCESS;

	IBTF_DPRINTF_L3(cmlog, "ibcm_saa_path_rec: done. Status = %d, "
	    "Found %d/%d Paths", retval, rec_found, *max_count);

	*max_count = rec_found; /* Update the return count. */

	return (retval);
}

ibt_status_t
ibcm_contact_sa_access(ibmf_saa_handle_t saa_handle,
    ibmf_saa_access_args_t *access_args, size_t *length, void **results_p)
{
	int	retry;
	int	sa_retval;

	IBTF_DPRINTF_L3(cmlog, "ibcm_contact_sa_access(%p, %p)",
	    saa_handle, access_args);

	ibcm_sa_access_enter();

	for (retry = 0; retry < ibcm_max_sa_retries; retry++) {
		sa_retval = ibmf_sa_access(saa_handle, access_args, 0,
		    length, results_p);
		if (sa_retval != IBMF_TRANS_TIMEOUT)
			break;

		IBTF_DPRINTF_L2(cmlog, "ibcm_contact_sa_access: "
		    "ibmf_sa_access() - Timed Out (%d)", sa_retval);
		delay(ibcm_sa_timeout_delay);
	}

	ibcm_sa_access_exit();

	if ((sa_retval == IBMF_SUCCESS) || (sa_retval == IBMF_NO_RECORDS) ||
	    (sa_retval == IBMF_REQ_INVALID)) {
		IBTF_DPRINTF_L3(cmlog, "ibcm_contact_sa_access: "
		    "ibmf_sa_access() returned (%d)", sa_retval);
		return (IBT_SUCCESS);
	} else  {
		IBTF_DPRINTF_L2(cmlog, "ibcm_contact_sa_access: "
		    "ibmf_sa_access(): Failed (%d)", sa_retval);
		return (ibcm_ibmf_analyze_error(sa_retval));
	}
}


static ibt_status_t
ibcm_update_pri(sa_path_record_t *pr_resp, ibtl_cm_port_list_t *sl,
    ibcm_dinfo_t *dinfo, ibt_path_info_t *paths)
{
	ibt_status_t	retval = IBT_SUCCESS;
	int		d, s;

	retval = ibcm_update_cep_info(pr_resp, sl, NULL,
	    &paths->pi_prim_cep_path);
	if (retval != IBT_SUCCESS)
		return (retval);

	/* Update some leftovers */
	paths->pi_prim_pkt_lt = pr_resp->PacketLifeTime;
	paths->pi_path_mtu = pr_resp->Mtu;

	for (d = 0; d < dinfo->num_dest; d++) {
		if (pr_resp->DGID.gid_guid == dinfo->dest[d].d_gid.gid_guid) {
			paths->pi_sid = dinfo->dest[d].d_sid;
			if (paths->pi_sid != 0) {
				bcopy(&dinfo->dest[d].d_sdata,
				    &paths->pi_sdata, sizeof (ibt_srv_data_t));
			}
			break;
		}
	}

	for (s = 0; s < sl->p_count; s++) {
		if (pr_resp->SGID.gid_guid == sl[s].p_sgid.gid_guid) {
			paths->pi_hca_guid = sl[s].p_hca_guid;
		}
	}

	/* Set Alternate Path to invalid state. */
	paths->pi_alt_cep_path.cep_hca_port_num = 0;
	paths->pi_alt_cep_path.cep_adds_vect.av_dlid = 0;

	IBTF_DPRINTF_L5(cmlog, "Path: HCA GUID  = 0x%llX", paths->pi_hca_guid);
	IBTF_DPRINTF_L5(cmlog, "Path: ServiceID = 0x%llX", paths->pi_sid);

	return (retval);
}


static ibt_status_t
ibcm_get_single_pathrec(ibcm_path_tqargs_t *p_arg, ibtl_cm_port_list_t *sl,
    ibcm_dinfo_t *dinfo, uint8_t idx, uint8_t *num_path, ibt_path_info_t *paths)
{
	sa_path_record_t	pathrec_req;
	sa_path_record_t	*pr_resp;
	ibmf_saa_access_args_t	access_args;
	uint64_t		c_mask = 0;
	void			*results_p;
	uint8_t			num_rec;
	size_t			length;
	ibt_status_t		retval;
	int			i, j, k;
	int			found, p_fnd;
	ibt_path_attr_t		*attrp = &p_arg->attr;
	ibmf_saa_handle_t	saa_handle;

	IBTF_DPRINTF_L3(cmlog, "ibcm_get_single_pathrec(%p, %p, %p, %d)",
	    p_arg, sl, dinfo, *num_path);

	bzero(&pathrec_req, sizeof (sa_path_record_t));

	/* Is Flow Label Specified. */
	if (attrp->pa_flow) {
		pathrec_req.FlowLabel = attrp->pa_flow;
		c_mask |= SA_PR_COMPMASK_FLOWLABEL;
	}

	/* Is HopLimit Specified. */
	if (p_arg->flags & IBT_PATH_HOP) {
		pathrec_req.HopLimit = attrp->pa_hop;
		c_mask |= SA_PR_COMPMASK_HOPLIMIT;
	}

	/* Is P_Key Specified. */
	if (dinfo->p_key) {
		IBTF_DPRINTF_L3(cmlog, "ibcm_get_single_pathrec: "
		    "Specified or Global PKEY 0x%X", dinfo->p_key);
		pathrec_req.P_Key = dinfo->p_key;
		c_mask |= SA_PR_COMPMASK_PKEY;
	}

	/* Is TClass Specified. */
	if (attrp->pa_tclass) {
		pathrec_req.TClass = attrp->pa_tclass;
		c_mask |= SA_PR_COMPMASK_TCLASS;
	}

	/* Is SL specified. */
	if (attrp->pa_sl) {
		pathrec_req.SL = attrp->pa_sl;
		c_mask |= SA_PR_COMPMASK_SL;
	}

	/* If IBT_PATH_PERF is set, then mark all selectors to BEST. */
	if (p_arg->flags & IBT_PATH_PERF) {
		pathrec_req.PacketLifeTimeSelector = IBT_BEST;
		pathrec_req.MtuSelector = IBT_BEST;
		pathrec_req.RateSelector = IBT_BEST;

		c_mask |= SA_PR_COMPMASK_PKTLTSELECTOR |
		    SA_PR_COMPMASK_RATESELECTOR | SA_PR_COMPMASK_MTUSELECTOR;
	} else {
		if (attrp->pa_pkt_lt.p_selector == IBT_BEST) {
			pathrec_req.PacketLifeTimeSelector = IBT_BEST;
			c_mask |= SA_PR_COMPMASK_PKTLTSELECTOR;
		}

		if (attrp->pa_srate.r_selector == IBT_BEST) {
			pathrec_req.RateSelector = IBT_BEST;
			c_mask |= SA_PR_COMPMASK_RATESELECTOR;
		}

		if (attrp->pa_mtu.r_selector == IBT_BEST) {
			pathrec_req.MtuSelector = IBT_BEST;
			c_mask |= SA_PR_COMPMASK_MTUSELECTOR;
		}
	}

	/*
	 * Honor individual selection of these attributes,
	 * even if IBT_PATH_PERF is set.
	 */
	/* Check out whether Packet Life Time is specified. */
	if (attrp->pa_pkt_lt.p_pkt_lt) {
		pathrec_req.PacketLifeTime =
		    ibt_usec2ib(attrp->pa_pkt_lt.p_pkt_lt);
		pathrec_req.PacketLifeTimeSelector =
		    attrp->pa_pkt_lt.p_selector;

		c_mask |= SA_PR_COMPMASK_PKTLT | SA_PR_COMPMASK_PKTLTSELECTOR;
	}

	/* Is SRATE specified. */
	if (attrp->pa_srate.r_srate) {
		pathrec_req.Rate = attrp->pa_srate.r_srate;
		pathrec_req.RateSelector = attrp->pa_srate.r_selector;

		c_mask |= SA_PR_COMPMASK_RATE | SA_PR_COMPMASK_RATESELECTOR;
	}

	/* Is MTU specified. */
	if (attrp->pa_mtu.r_mtu) {
		pathrec_req.Mtu = attrp->pa_mtu.r_mtu;
		pathrec_req.MtuSelector = attrp->pa_mtu.r_selector;

		c_mask |= SA_PR_COMPMASK_MTU | SA_PR_COMPMASK_MTUSELECTOR;
	}

	/* We always get REVERSIBLE paths. */
	pathrec_req.Reversible = 1;
	c_mask |= SA_PR_COMPMASK_REVERSIBLE;

	pathrec_req.NumbPath = *num_path;
	c_mask |= SA_PR_COMPMASK_NUMBPATH;

	if (idx != 0xFF) {
		/* MGID */
		pathrec_req.DGID = dinfo->dest[idx].d_gid;
		c_mask |= SA_PR_COMPMASK_DGID;
	}

	p_fnd = found = 0;

	for (i = 0; i < sl->p_count; i++) {
		/* SGID */
		pathrec_req.SGID = sl[i].p_sgid;
		c_mask |= SA_PR_COMPMASK_SGID;
		saa_handle = sl[i].p_saa_hdl;

		for (k = 0; k < dinfo->num_dest; k++) {
			if (idx == 0xFF) {		/* DGID */
				if (dinfo->dest[k].d_tag != 0)
					continue;

				if (pathrec_req.SGID.gid_prefix !=
				    dinfo->dest[k].d_gid.gid_prefix) {
					IBTF_DPRINTF_L3(cmlog,
					    "ibcm_get_single_pathrec: SGID_pfx="
					    "%llX, DGID_pfx=%llX doesn't match",
					    pathrec_req.SGID.gid_prefix,
					    dinfo->dest[k].d_gid.gid_prefix);
					continue;
				} else if (pathrec_req.SGID.gid_guid ==
				    pathrec_req.DGID.gid_guid) {
					IBTF_DPRINTF_L3(cmlog,
					    "ibcm_get_single_pathrec: Why "
					    "LoopBack request came here!!!! "
					    "GID(%llX:%llX)",
					    pathrec_req.SGID.gid_prefix,
					    pathrec_req.SGID.gid_guid);
					continue;
				}

				pathrec_req.DGID = dinfo->dest[k].d_gid;
				c_mask |= SA_PR_COMPMASK_DGID;

				/*
				 * If we had performed Service Look-up, then we
				 * got P_Key from ServiceRecord, so get path
				 * records that satisfy this particular P_Key.
				 */
				if ((dinfo->p_key == 0) &&
				    (dinfo->dest[k].d_pkey != 0)) {
					pathrec_req.P_Key =
					    dinfo->dest[k].d_pkey;
					c_mask |= SA_PR_COMPMASK_PKEY;
				}
			}

			IBTF_DPRINTF_L3(cmlog, "ibcm_get_single_pathrec: "
			    "Get %d Path(s) between\n\tSGID(%llX:%llX) "
			    "DGID(%llX:%llX)", pathrec_req.NumbPath,
			    pathrec_req.SGID.gid_prefix,
			    pathrec_req.SGID.gid_guid,
			    pathrec_req.DGID.gid_prefix,
			    pathrec_req.DGID.gid_guid);

			IBTF_DPRINTF_L3(cmlog, "ibcm_get_single_pathrec: CMask"
			    "=0x%llX, PKey=0x%X", c_mask, pathrec_req.P_Key);

			/* Contact SA Access to retrieve Path Records. */
			access_args.sq_attr_id = SA_PATHRECORD_ATTRID;
			access_args.sq_template = &pathrec_req;
			access_args.sq_access_type = IBMF_SAA_RETRIEVE;
			access_args.sq_template_length =
			    sizeof (sa_path_record_t);
			access_args.sq_component_mask = c_mask;
			access_args.sq_callback = NULL;
			access_args.sq_callback_arg = NULL;

			retval = ibcm_contact_sa_access(saa_handle,
			    &access_args, &length, &results_p);
			if (retval != IBT_SUCCESS) {
				*num_path = 0;
				return (retval);
			}

			num_rec = length / sizeof (sa_path_record_t);

			IBTF_DPRINTF_L3(cmlog, "ibcm_get_single_pathrec: "
			    "FOUND %d/%d path requested", num_rec, *num_path);

			if ((results_p == NULL) || (num_rec == 0)) {
				if (idx != 0xFF)
					break;
				else
					continue;
			}

			/* Update the PathInfo from the response. */
			pr_resp = (sa_path_record_t *)results_p;
			for (j = 0; j < num_rec; j++, pr_resp++) {
				if ((p_fnd != 0) &&
				    (p_arg->flags & IBT_PATH_APM)) {
					IBTF_DPRINTF_L3(cmlog,
					    "ibcm_get_single_pathrec: "
					    "Fill Alternate Path");
					retval = ibcm_update_cep_info(pr_resp,
					    sl, NULL,
					    &paths[found - 1].pi_alt_cep_path);
					if (retval != IBT_SUCCESS)
						continue;

					/* Update some leftovers */
					paths[found - 1].pi_alt_pkt_lt =
					    pr_resp->PacketLifeTime;
					p_fnd = 0;
				} else {
					IBTF_DPRINTF_L3(cmlog,
					    "ibcm_get_single_pathrec: "
					    "Fill Primary Path");

					if (found == *num_path)
						break;

					retval = ibcm_update_pri(pr_resp, sl,
					    dinfo, &paths[found]);
					if (retval != IBT_SUCCESS)
						continue;
					p_fnd = 1;
					found++;
				}

			}
			/* Deallocate the memory for results_p. */
			kmem_free(results_p, length);

			if (idx != 0xFF)
				break;		/* We r here for MGID */
		}
		if ((idx != 0xFF) && (found == *num_path))
			break;		/* We r here for MGID */
	}

	if (found == 0)
		retval = IBT_PATH_RECORDS_NOT_FOUND;
	else if (found != *num_path)
		retval = IBT_INSUFF_DATA;
	else
		retval = IBT_SUCCESS;

	IBTF_DPRINTF_L3(cmlog, "ibcm_get_single_pathrec: done. Status %d, "
	    "Found %d/%d Paths", retval, found, *num_path);

	*num_path = found;

	return (retval);
}


static ibt_status_t
ibcm_get_multi_pathrec(ibcm_path_tqargs_t *p_arg, ibtl_cm_port_list_t *sl,
    ibcm_dinfo_t *dinfo, uint8_t *num_path, ibt_path_info_t *paths)
{
	sa_multipath_record_t	*mpr_req;
	sa_path_record_t	*pr_resp;
	ibmf_saa_access_args_t	access_args;
	void			*results_p;
	uint64_t		c_mask = 0;
	ib_gid_t		*gid_ptr, *gid_s_ptr;
	size_t			length;
	int			template_len, found, num_rec;
	int			i, k;
	ibt_status_t		retval;
	uint8_t			sgid_cnt, dgid_cnt;
	ibt_path_attr_t		*attrp = &p_arg->attr;

	IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec(%p, %p, %p, %d)",
	    attrp, sl, dinfo, *num_path);

	for (i = 0, dgid_cnt = 0; i < dinfo->num_dest; i++) {
		if (dinfo->dest[i].d_tag == 0)
			dgid_cnt++;
	}

	sgid_cnt = sl->p_count;

	if ((sgid_cnt == 0) || (dgid_cnt == 0)) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_get_multi_pathrec: sgid_cnt(%d) or"
		    " dgid_cnt(%d) is zero", sgid_cnt, dgid_cnt);
		return (IBT_INVALID_PARAM);
	}

	IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec: Get %d records between "
	    "%d Src(s) <=> %d Dest(s)", *num_path, sgid_cnt, dgid_cnt);

	/*
	 * Calculate the size for multi-path records template, which includes
	 * constant portion of the multipath record, plus variable size for
	 * SGID (sgid_cnt) and DGID (dgid_cnt).
	 */
	template_len = ((dgid_cnt + sgid_cnt) * sizeof (ib_gid_t)) +
	    sizeof (sa_multipath_record_t);

	mpr_req = kmem_zalloc(template_len, KM_SLEEP);

	ASSERT(mpr_req != NULL);

	gid_ptr = (ib_gid_t *)(((uchar_t *)mpr_req) +
	    sizeof (sa_multipath_record_t));

	/* Get the starting pointer where GIDs are stored. */
	gid_s_ptr = gid_ptr;

	/* SGID */
	for (i = 0; i < sl->p_count; i++) {
		*gid_ptr = sl[i].p_sgid;

		IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec: SGID[%d] = "
		    "(%llX:%llX)", i, gid_ptr->gid_prefix, gid_ptr->gid_guid);

		gid_ptr++;
	}

	_NOTE(NOW_INVISIBLE_TO_OTHER_THREADS(*mpr_req))

	mpr_req->SGIDCount = sgid_cnt;
	c_mask = SA_MPR_COMPMASK_SGIDCOUNT;

	/* DGIDs */
	for (i = 0; i < dinfo->num_dest; i++) {
		if (dinfo->dest[i].d_tag == 0) {
			*gid_ptr = dinfo->dest[i].d_gid;

			IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec: "
			    "DGID[%d] = (%llX:%llX)", i, gid_ptr->gid_prefix,
			    gid_ptr->gid_guid);
			gid_ptr++;
		}
	}

	mpr_req->DGIDCount = dgid_cnt;
	c_mask |= SA_MPR_COMPMASK_DGIDCOUNT;

	/* Is Flow Label Specified. */
	if (attrp->pa_flow) {
		mpr_req->FlowLabel = attrp->pa_flow;
		c_mask |= SA_MPR_COMPMASK_FLOWLABEL;
	}

	/* Is HopLimit Specified. */
	if (p_arg->flags & IBT_PATH_HOP) {
		mpr_req->HopLimit = attrp->pa_hop;
		c_mask |= SA_MPR_COMPMASK_HOPLIMIT;
	}

	/* Is TClass Specified. */
	if (attrp->pa_tclass) {
		mpr_req->TClass = attrp->pa_tclass;
		c_mask |= SA_MPR_COMPMASK_TCLASS;
	}

	/* Is SL specified. */
	if (attrp->pa_sl) {
		mpr_req->SL = attrp->pa_sl;
		c_mask |= SA_MPR_COMPMASK_SL;
	}

	if (p_arg->flags & IBT_PATH_PERF) {
		mpr_req->PacketLifeTimeSelector = IBT_BEST;
		mpr_req->RateSelector = IBT_BEST;
		mpr_req->MtuSelector = IBT_BEST;

		c_mask |= SA_MPR_COMPMASK_PKTLTSELECTOR |
		    SA_MPR_COMPMASK_RATESELECTOR | SA_MPR_COMPMASK_MTUSELECTOR;
	} else {
		if (attrp->pa_pkt_lt.p_selector == IBT_BEST) {
			mpr_req->PacketLifeTimeSelector = IBT_BEST;
			c_mask |= SA_MPR_COMPMASK_PKTLTSELECTOR;
		}

		if (attrp->pa_srate.r_selector == IBT_BEST) {
			mpr_req->RateSelector = IBT_BEST;
			c_mask |= SA_MPR_COMPMASK_RATESELECTOR;
		}

		if (attrp->pa_mtu.r_selector == IBT_BEST) {
			mpr_req->MtuSelector = IBT_BEST;
			c_mask |= SA_MPR_COMPMASK_MTUSELECTOR;
		}
	}

	/*
	 * Honor individual selection of these attributes,
	 * even if IBT_PATH_PERF is set.
	 */
	/* Check out whether Packet Life Time is specified. */
	if (attrp->pa_pkt_lt.p_pkt_lt) {
		mpr_req->PacketLifeTime =
		    ibt_usec2ib(attrp->pa_pkt_lt.p_pkt_lt);
		mpr_req->PacketLifeTimeSelector =
		    attrp->pa_pkt_lt.p_selector;

		c_mask |= SA_MPR_COMPMASK_PKTLT |
		    SA_MPR_COMPMASK_PKTLTSELECTOR;
	}

	/* Is SRATE specified. */
	if (attrp->pa_srate.r_srate) {
		mpr_req->Rate = attrp->pa_srate.r_srate;
		mpr_req->RateSelector = attrp->pa_srate.r_selector;

		c_mask |= SA_MPR_COMPMASK_RATE |
		    SA_MPR_COMPMASK_RATESELECTOR;
	}

	/* Is MTU specified. */
	if (attrp->pa_mtu.r_mtu) {
		mpr_req->Mtu = attrp->pa_mtu.r_mtu;
		mpr_req->MtuSelector = attrp->pa_mtu.r_selector;

		c_mask |= SA_MPR_COMPMASK_MTU |
		    SA_MPR_COMPMASK_MTUSELECTOR;
	}

	/* Is P_Key Specified or obtained during Service Look-up. */
	if (dinfo->p_key) {
		mpr_req->P_Key = dinfo->p_key;
		c_mask |= SA_MPR_COMPMASK_PKEY;
	}

	/* We always get REVERSIBLE paths. */
	mpr_req->Reversible = 1;
	c_mask |= SA_MPR_COMPMASK_REVERSIBLE;

	if (p_arg->flags & IBT_PATH_AVAIL) {
		mpr_req->IndependenceSelector = 1;
		c_mask |= SA_MPR_COMPMASK_INDEPSEL;
	}

	/* we will not specify how many records we want. */

	_NOTE(NOW_VISIBLE_TO_OTHER_THREADS(*mpr_req))

	IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec: CMask: %llX Pkey: %X",
	    c_mask, mpr_req->P_Key);

	/* Contact SA Access to retrieve Path Records. */
	access_args.sq_attr_id = SA_MULTIPATHRECORD_ATTRID;
	access_args.sq_access_type = IBMF_SAA_RETRIEVE;
	access_args.sq_component_mask = c_mask;
	access_args.sq_template = mpr_req;
	access_args.sq_template_length = sizeof (sa_multipath_record_t);
	access_args.sq_callback = NULL;
	access_args.sq_callback_arg = NULL;

	retval = ibcm_contact_sa_access(sl->p_saa_hdl, &access_args, &length,
	    &results_p);
	if (retval != IBT_SUCCESS) {
		*num_path = 0;  /* Update the return count. */
		kmem_free(mpr_req, template_len);
		return (retval);
	}

	num_rec = length / sizeof (sa_path_record_t);

	IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec: Found %d Paths",
	    num_rec);

	found = 0;
	if ((results_p != NULL) && (num_rec > 0)) {
		/* Update the PathInfo with the response Path Records */
		pr_resp = (sa_path_record_t *)results_p;

		for (i = 0; i < num_rec; i++) {
			IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec: "
			    "P[%d]: SG %llX, DG %llX", i,
			    pr_resp[i].SGID.gid_guid, pr_resp[i].DGID.gid_guid);
		}

		if (p_arg->flags & IBT_PATH_APM) {
			sa_path_record_t *p_resp = NULL, *a_resp = NULL;
			int		p_found = 0, a_found = 0;
			ib_gid_t	p_sg, a_sg, p_dg, a_dg;
			int		s_spec;

			s_spec = p_arg->attr.pa_sgid.gid_guid != 0 ? 1 : 0;

			p_sg = gid_s_ptr[0];
			if (sgid_cnt > 1)
				a_sg = gid_s_ptr[1];
			else
				a_sg = p_sg;

			IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec: "
			    "REQ: P_SG: %llX, A_SG: %llX",
			    p_sg.gid_guid, a_sg.gid_guid);

			p_dg = gid_s_ptr[sgid_cnt];
			if (dgid_cnt > 1)
				a_dg = gid_s_ptr[sgid_cnt + 1];
			else
				a_dg = p_dg;

			IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec: "
			    "REQ: P_DG: %llX, A_DG: %llX",
			    p_dg.gid_guid, a_dg.gid_guid);

			/*
			 * If SGID and/or DGID is specified by user, make sure
			 * he gets his primary-path on those node points.
			 */
			for (i = 0; i < num_rec; i++, pr_resp++) {
				IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec:"
				    " PF %d, AF %d,\n\t\t P[%d] = SG: %llX, "
				    "DG: %llX", p_found, a_found, i,
				    pr_resp->SGID.gid_guid,
				    pr_resp->DGID.gid_guid);

				if ((!p_found) &&
				    (p_dg.gid_guid == pr_resp->DGID.gid_guid)) {
					IBTF_DPRINTF_L3(cmlog,
					    "ibcm_get_multi_pathrec: "
					    "Pri DGID Match.. ");
					if ((s_spec == 0) || (p_sg.gid_guid ==
					    pr_resp->SGID.gid_guid)) {
						p_found = 1;
						p_resp = pr_resp;
						IBTF_DPRINTF_L3(cmlog,
						    "ibcm_get_multi_pathrec: "
						    "Primary Path Found");

						if (a_found)
							break;
						else
							continue;
					}
					IBTF_DPRINTF_L3(cmlog,
					    "ibcm_get_multi_pathrec:"
					    "Pri SGID Don't Match.. ");
				}

				if ((!a_found) &&
				    (a_dg.gid_guid == pr_resp->DGID.gid_guid)) {
					IBTF_DPRINTF_L3(cmlog,
					    "ibcm_get_multi_pathrec:"
					    "Alt DGID Match.. ");
					if ((s_spec == 0) || (a_sg.gid_guid ==
					    pr_resp->SGID.gid_guid)) {
						a_found = 1;
						a_resp = pr_resp;

						IBTF_DPRINTF_L3(cmlog,
						    "ibcm_get_multi_pathrec:"
						    "Alternate Path Found ");

						if (p_found)
							break;
						else
							continue;
					}
					IBTF_DPRINTF_L3(cmlog,
					    "ibcm_get_multi_pathrec:"
					    "Alt SGID Don't Match.. ");
				}
			}

			if ((p_found == 0) && (a_found == 0)) {
				IBTF_DPRINTF_L2(cmlog, "ibcm_get_multi_pathrec:"
				    " Path to desired node points NOT "
				    "Available.");
				retval = IBT_PATH_RECORDS_NOT_FOUND;
				goto get_multi_pathrec_end;
			}

			if ((p_resp == NULL) && (a_resp != NULL)) {
				p_resp = a_resp;
				a_resp = NULL;
			}

			/* Fill in Primary Path */
			retval = ibcm_update_pri(p_resp, sl, dinfo,
			    &paths[found]);
			if (retval != IBT_SUCCESS)
				goto get_multi_pathrec_end;

			/* Fill in Alternate Path */
			if (a_resp != NULL) {
				/* a_resp will point to AltPathInfo buffer. */
				retval = ibcm_update_cep_info(a_resp, sl,
				    NULL, &paths[found].pi_alt_cep_path);
				if (retval != IBT_SUCCESS)
					goto get_multi_pathrec_end;

				/* Update some leftovers */
				paths[found].pi_alt_pkt_lt =
				    a_resp->PacketLifeTime;
			} else {
				IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec:"
				    " Alternate Path NOT Available.");
				retval = IBT_INSUFF_DATA;
			}
			found++;
		} else {	/* If NOT APM */
			boolean_t	check_pkey = B_FALSE;

			/* mark flag whether to validate PKey or not. */
			if ((dinfo->p_key == 0) && (dinfo->dest[0].d_pkey != 0))
				check_pkey = B_TRUE;

			for (i = 0; i < num_rec; i++, pr_resp++) {
				IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec:"
				    " PKeyCheck - %s, PKey=0x%X, DGID(%llX)",
				    ((check_pkey == B_TRUE)?"REQD":"NOT_REQD"),
				    pr_resp->P_Key, pr_resp->DGID.gid_guid);

				if (check_pkey == B_TRUE) {
					boolean_t	match_found = B_FALSE;

					/* For all DGIDs */
					for (k = 0; k < dinfo->num_dest; k++) {
						if (dinfo->dest[k].d_tag != 0)
							continue;

						if ((dinfo->dest[k].d_gid.
						    gid_guid ==
						    pr_resp->DGID.gid_guid) &&
						    (dinfo->dest[k].d_pkey ==
						    pr_resp->P_Key)) {
							match_found = B_TRUE;
							break;
						}
					}
					if (match_found == B_FALSE)
						continue;
				}
				/* Fill in Primary Path */
				retval = ibcm_update_pri(pr_resp, sl, dinfo,
				    &paths[found]);
				if (retval != IBT_SUCCESS)
					continue;

				if (++found == *num_path)
					break;
			}
		}
get_multi_pathrec_end:
		kmem_free(results_p, length);
	}
	kmem_free(mpr_req, template_len);

	if (found == 0)
		retval = IBT_PATH_RECORDS_NOT_FOUND;
	else if (found != *num_path)
		retval = IBT_INSUFF_DATA;
	else
		retval = IBT_SUCCESS;

	IBTF_DPRINTF_L3(cmlog, "ibcm_get_multi_pathrec: Done (status %d). "
	    "Found %d/%d Paths", retval, found, *num_path);

	*num_path = found;	/* Update the return count. */

	return (retval);
}


/*
 * Here we "synthesize" loop back path record information.
 *
 * Currently the synthesize values are assumed as follows:
 *    SLID, DLID = Base LID from Query HCA Port.
 *    FlowLabel, HopLimit, TClass = 0, as GRH is False.
 *    RawTraffic = 0.
 *    P_Key = first valid one in P_Key table as obtained from Query HCA Port.
 *    SL = as from Query HCA Port.
 *    MTU = from Query HCA Port.
 *    Rate = 2 (arbitrary).
 *    PacketLifeTime = 0 (4.096 usec).
 */
static ibt_status_t
ibcm_fillin_loopbackinfo(ibtl_cm_port_list_t *sl, uint8_t index,
    ibcm_dinfo_t *dinfo, ibt_path_info_t *paths)
{
	ibt_status_t	retval;
	ib_pkey_t	pkey = 0;

	IBTF_DPRINTF_L3(cmlog, "ibcm_fillin_loopbackinfo(%p, %p)", sl, dinfo);

	/* Synthesize path record with appropriate loop back information. */
	if (dinfo->p_key)
		pkey = dinfo->p_key;
	else
		pkey = dinfo->dest[index].d_pkey;
	if (pkey) {
		/* Convert P_Key to P_Key_Index */
		retval = ibt_pkey2index_byguid(sl->p_hca_guid, sl->p_port_num,
		    pkey, &paths->pi_prim_cep_path.cep_pkey_ix);
		if (retval != IBT_SUCCESS) {
			/* Failed to get pkey_index from pkey */
			IBTF_DPRINTF_L2(cmlog, "ibcm_fillin_loopbackinfo: "
			    "Pkey2Index (P_Key = %X) conversion failed: %d",
			    pkey, retval);
			return (retval);
		}
	} else {
		paths->pi_prim_cep_path.cep_pkey_ix =
		    ibtl_cm_get_1st_full_pkey_ix(sl->p_hca_guid,
		    sl->p_port_num);
		IBTF_DPRINTF_L3(cmlog, "ibcm_fillin_loopbackinfo: "
		    "1st Full Member P_Key_ix = %d",
		    paths->pi_prim_cep_path.cep_pkey_ix);
	}

	paths->pi_hca_guid = sl->p_hca_guid;
	paths->pi_prim_cep_path.cep_adds_vect.av_dgid =
	    dinfo->dest[index].d_gid;
	paths->pi_prim_cep_path.cep_adds_vect.av_sgid = sl->p_sgid;
	paths->pi_prim_cep_path.cep_adds_vect.av_srate	= IBT_SRATE_1X;
	paths->pi_prim_cep_path.cep_adds_vect.av_srvl	= 0; /* SL */

	paths->pi_prim_cep_path.cep_adds_vect.av_send_grh = B_FALSE;
	paths->pi_prim_cep_path.cep_adds_vect.av_flow	= 0;
	paths->pi_prim_cep_path.cep_adds_vect.av_tclass	= 0;
	paths->pi_prim_cep_path.cep_adds_vect.av_hop 	= 0;

	/* SLID and DLID will be equal to BLID. */
	paths->pi_prim_cep_path.cep_adds_vect.av_dlid = sl->p_base_lid;
	paths->pi_prim_cep_path.cep_adds_vect.av_src_path = 0;
	paths->pi_prim_cep_path.cep_adds_vect.av_sgid_ix = sl->p_sgid_ix;
	paths->pi_prim_cep_path.cep_adds_vect.av_port_num =
	paths->pi_prim_cep_path.cep_hca_port_num = sl->p_port_num;
	paths->pi_prim_cep_path.cep_timeout = 0; /* To be filled in by CM. */
	paths->pi_path_mtu = sl->p_mtu;		/* MTU */
	paths->pi_prim_pkt_lt = 0;		/* Packet Life Time. */
	paths->pi_alt_pkt_lt = 0;		/* Packet Life Time. */

	paths->pi_sid = dinfo->dest[index].d_sid;

	if (paths->pi_sid != 0)
		bcopy(&dinfo->dest[index].d_sdata, &paths->pi_sdata,
		    sizeof (ibt_srv_data_t));

	IBTF_DPRINTF_L3(cmlog, "ibcm_fillin_loopbackinfo: HCA %llX:%d SID %llX"
	    "\n\t SGID %llX:%llX DGID %llX:%llX", paths->pi_hca_guid,
	    paths->pi_prim_cep_path.cep_hca_port_num, paths->pi_sid,
	    sl->p_sgid.gid_prefix, sl->p_sgid.gid_guid,
	    dinfo->dest[index].d_gid.gid_prefix,
	    dinfo->dest[index].d_gid.gid_guid);

	/* Set Alternate Path to invalid state. */
	paths->pi_alt_cep_path.cep_hca_port_num = 0;
	paths->pi_alt_cep_path.cep_adds_vect.av_dlid = 0;

	return (IBT_SUCCESS);
}


/*
 * Update the output path records buffer with the values as obtained from
 * SA Access retrieve call results for Path Records.
 */
static ibt_status_t
ibcm_update_cep_info(sa_path_record_t *prec_resp, ibtl_cm_port_list_t *sl,
    ibtl_cm_hca_port_t *hport, ibt_cep_path_t *cep_p)
{
	ibt_status_t	retval;
	int		i;

	IBCM_DUMP_PATH_REC(prec_resp);

	/*
	 * If path's packet life time is more than 4 seconds, IBCM cannot
	 * handle this path connection, so discard this path record.
	 */
	if (prec_resp->PacketLifeTime > ibcm_max_ib_pkt_lt) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_update_cep_info: Path's Packet "
		    "LifeTime too high %d, Maximum allowed %d IB Time (4 sec)",
		    prec_resp->PacketLifeTime, ibcm_max_ib_pkt_lt);
		return (IBT_PATH_PKT_LT_TOO_HIGH);
	}

	if ((prec_resp->Mtu > IB_MTU_4K) || (prec_resp->Mtu < IB_MTU_256)) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_update_cep_info: MTU (%d) from "
		    "pathrecord is invalid, reject it.", prec_resp->Mtu);
		return (ibt_get_module_failure(IBT_FAILURE_IBCM, 0));
	}

	/* Source Node Information. */
	cep_p->cep_adds_vect.av_sgid = prec_resp->SGID;
	if (hport != NULL) {
		/* Convert P_Key to P_Key_Index */
		retval = ibt_pkey2index_byguid(hport->hp_hca_guid,
		    hport->hp_port, prec_resp->P_Key, &cep_p->cep_pkey_ix);
		if (retval != IBT_SUCCESS) {
			/* Failed to get pkey_index from pkey */
			IBTF_DPRINTF_L2(cmlog, "ibcm_update_cep_info: "
			    "Pkey2Index conversion failed: %d", retval);
			return (retval);
		}
		cep_p->cep_adds_vect.av_sgid_ix = hport->hp_sgid_ix;
		cep_p->cep_adds_vect.av_src_path =
		    prec_resp->SLID - hport->hp_base_lid;
		cep_p->cep_adds_vect.av_port_num = cep_p->cep_hca_port_num =
		    hport->hp_port;
	} else if (sl != NULL) {
		for (i = 0; i < sl->p_count; i++) {
			if (prec_resp->SGID.gid_guid == sl[i].p_sgid.gid_guid) {
				/* Convert P_Key to P_Key_Index */
				retval = ibt_pkey2index_byguid(sl[i].p_hca_guid,
				    sl[i].p_port_num, prec_resp->P_Key,
				    &cep_p->cep_pkey_ix);
				if (retval != IBT_SUCCESS) {
					/* Failed to get pkey_index from pkey */
					IBTF_DPRINTF_L2(cmlog,
					    "ibcm_update_cep_info: Pkey2Index "
					    "conversion failed: %d", retval);
					return (retval);
				}

				cep_p->cep_adds_vect.av_sgid_ix =
				    sl[i].p_sgid_ix;
				cep_p->cep_adds_vect.av_src_path =
				    prec_resp->SLID - sl[i].p_base_lid;
				cep_p->cep_adds_vect.av_port_num =
				    sl[i].p_port_num;
				cep_p->cep_hca_port_num = sl[i].p_port_num;

				break;
			}
		}
	} else {
		IBTF_DPRINTF_L2(cmlog, "ibcm_update_cep_info: Sl or Hport "
		    "must be non-null");
		return (IBT_INVALID_PARAM);
	}

	switch (prec_resp->Rate) {
	case IBT_SRATE_1X:
	case IBT_SRATE_4X:
	case IBT_SRATE_12X:
		cep_p->cep_adds_vect.av_srate = prec_resp->Rate;
		break;
	default:
		IBTF_DPRINTF_L2(cmlog, "ibcm_update_cep_info: SRate (%d) from "
		    "pathrecord is invalid, reject it.", prec_resp->Rate);
		return (IBT_STATIC_RATE_INVALID);
	}
	/*
	 * If both Source and Destination GID prefix are same, then GRH is not
	 * valid, so make it as false, else set this field as true.
	 */
	if (prec_resp->SGID.gid_prefix == prec_resp->DGID.gid_prefix)
		cep_p->cep_adds_vect.av_send_grh = B_FALSE;
	else
		cep_p->cep_adds_vect.av_send_grh = B_TRUE;

	/* SGID and SGID Index. */
	cep_p->cep_adds_vect.av_sgid = prec_resp->SGID;
	cep_p->cep_adds_vect.av_flow = prec_resp->FlowLabel;
	cep_p->cep_adds_vect.av_tclass = prec_resp->TClass;
	cep_p->cep_adds_vect.av_hop = prec_resp->HopLimit;

	/* Address Vector Definition. */
	cep_p->cep_adds_vect.av_dlid = prec_resp->DLID;
	cep_p->cep_adds_vect.av_srvl = prec_resp->SL;

	/* DGID */
	cep_p->cep_adds_vect.av_dgid = prec_resp->DGID;

	/* CEP Timeout is NOT filled in by PATH routines. */
	cep_p->cep_timeout = 0;

	IBTF_DPRINTF_L3(cmlog, "ibcm_update_cep_info: Done.  Port[%d]\n"
	    "SGID=%llX:%llX DGID=%llX:%llX",
	    cep_p->cep_adds_vect.av_port_num,
	    prec_resp->SGID.gid_prefix, prec_resp->SGID.gid_guid,
	    prec_resp->DGID.gid_prefix, prec_resp->DGID.gid_guid);

	return (IBT_SUCCESS);
}


static void
ibcm_fill_svcinfo(sa_service_record_t *sr_resp, ibcm_dest_t *dest)
{
	_NOTE(NOW_INVISIBLE_TO_OTHER_THREADS(*dest))

	dest->d_gid = sr_resp->ServiceGID;
	dest->d_sid = sr_resp->ServiceID;
	ibcm_swizzle_to_srv(sr_resp->ServiceData, &dest->d_sdata);
	dest->d_pkey = sr_resp->ServiceP_Key;

	_NOTE(NOW_VISIBLE_TO_OTHER_THREADS(*dest))

	IBTF_DPRINTF_L3(cmlog, "ibcm_fill_svcinfo: SID(%llX), GID(%llX:%llX)"
	    "\n\tSvcPKey 0x%X", dest->d_sid, dest->d_gid.gid_prefix,
	    dest->d_gid.gid_guid, dest->d_pkey);
}


static ib_gid_t
ibcm_saa_get_agid(ibtl_cm_port_list_t *sl, ib_gid_t *gidp, uint_t ngid)
{
	int		k, l;
	ib_gid_t	a_gid;

	a_gid.gid_prefix = a_gid.gid_guid = 0;

	for (k = 0; k < sl->p_count; k++) {
		for (l = 0; l < ngid; l++) {

			if (gidp->gid_prefix == sl->p_sgid.gid_prefix) {
				a_gid = *gidp;
				break;
			}
			if (a_gid.gid_guid && a_gid.gid_prefix)
				break;
			gidp++;
		}
		if (a_gid.gid_guid && a_gid.gid_prefix)
			break;
		sl++;
	}
	IBTF_DPRINTF_L3(cmlog, "ibcm_saa_get_agid: AltGID = %llX:%llX",
	    a_gid.gid_prefix, a_gid.gid_guid);

	return (a_gid);
}

/*
 * Perform SA Access to retrieve Service Records.
 * On Success, returns ServiceID and ServiceGID info in '*dinfo'.
 */
static ibt_status_t
ibcm_saa_service_rec(ibcm_path_tqargs_t *p_arg, ibtl_cm_port_list_t *sl,
    ibcm_dinfo_t *dinfo)
{
	sa_service_record_t	svcrec_req;
	sa_service_record_t	*svcrec_resp;
	void			*results_p;
	uint64_t		component_mask = 0;
	size_t			length;
	uint8_t			i, j, k, rec_found, s;
	ibmf_saa_access_args_t	access_args;
	ibt_status_t		retval;
	ibt_path_attr_t		*attrp = &p_arg->attr;
	uint64_t		tmp_sd_flag = attrp->pa_sd_flags;
	uint8_t			num_req;

	IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec(%p, %p)", p_arg, sl);

	bzero(&svcrec_req, sizeof (svcrec_req));

	/* Service Name */
	if ((attrp->pa_sname != NULL) && (strlen(attrp->pa_sname) != 0)) {
		(void) strncpy((char *)(svcrec_req.ServiceName),
		    attrp->pa_sname, IB_SVC_NAME_LEN);

		component_mask |= SA_SR_COMPMASK_NAME;
	}

	/* Service ID */
	if (attrp->pa_sid) {
		svcrec_req.ServiceID = attrp->pa_sid;
		component_mask |= SA_SR_COMPMASK_ID;
	}

	/* Is P_Key Specified. */
	if (p_arg->flags & IBT_PATH_PKEY) {
		svcrec_req.ServiceP_Key = attrp->pa_pkey;
		component_mask |= SA_SR_COMPMASK_PKEY;
	}

	/* Is ServiceData Specified. */
	if (attrp->pa_sd_flags != IBT_NO_SDATA) {
		/* Handle endianess for service data. */
		ibcm_swizzle_from_srv(&attrp->pa_sdata, svcrec_req.ServiceData);

		/*
		 * Lets not interpret each and every ServiceData flags,
		 * just pass it on to SAA. Shift the flag, to suit
		 * SA_SR_COMPMASK_ALL_DATA definition.
		 */
		component_mask |= (tmp_sd_flag << 7);
	}

	if (dinfo->num_dest == 1) {

		/* If a single DGID is specified, provide it */
		svcrec_req.ServiceGID = dinfo->dest->d_gid;
		component_mask |= SA_SR_COMPMASK_GID;

		IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec:%llX:%llX",
		    svcrec_req.ServiceGID.gid_prefix,
		    svcrec_req.ServiceGID.gid_guid);
	}

	IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: "
	    "Perform SA Access: Mask: 0x%X", component_mask);

	/*
	 * Call in SA Access retrieve routine to get Service Records.
	 *
	 * SA Access framework allocated memory for the "results_p".
	 * Make sure to deallocate once we are done with the results_p.
	 * The size of the buffer allocated will be as returned in
	 * "length" field.
	 */
	access_args.sq_attr_id = SA_SERVICERECORD_ATTRID;
	access_args.sq_access_type = IBMF_SAA_RETRIEVE;
	access_args.sq_component_mask = component_mask;
	access_args.sq_template = &svcrec_req;
	access_args.sq_template_length = sizeof (sa_service_record_t);
	access_args.sq_callback = NULL;
	access_args.sq_callback_arg = NULL;

	for (s = 0; s < sl->p_count; s++) {
		retval = ibcm_contact_sa_access(sl[s].p_saa_hdl, &access_args,
		    &length, &results_p);
		if (retval != IBT_SUCCESS)
			if (sl[s].p_multi & IBTL_CM_MULTI_SM)
				continue;
			else
				return (retval);

		if ((results_p == NULL) || (length == 0)) {
			IBTF_DPRINTF_L2(cmlog, "ibcm_saa_service_rec: SvcRec "
			    "Not Found: res_p %p, len %d", results_p, length);
			if (sl[s].p_multi & IBTL_CM_MULTI_SM) {
				retval = IBT_SERVICE_RECORDS_NOT_FOUND;
				continue;
			} else
				return (IBT_SERVICE_RECORDS_NOT_FOUND);
		}

		/* if we are here, we got some records. so break. */
		break;
	}

	if (retval != IBT_SUCCESS)
		return (retval);

	num_req = length / sizeof (sa_service_record_t);

	IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: Got %d Service Records.",
	    num_req);

	svcrec_resp = (sa_service_record_t *)results_p;
	rec_found = 0;

	/* Update the return values. */
	if (dinfo->num_dest) {
		IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: Get ServiceRec "
		    "for Specified DGID: %d", dinfo->num_dest);

		for (i = 0; i < num_req; i++, svcrec_resp++) {
			/* Limited P_Key is NOT supported as of now!. */
			if ((svcrec_resp->ServiceP_Key & 0x8000) == 0) {
				IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: "
				    "SvcPkey 0x%X limited, reject the record.",
				    svcrec_resp->ServiceP_Key);
				continue;
			}

			for (j = 0; j < dinfo->num_dest; j++) {
				if (dinfo->dest[j].d_gid.gid_guid ==
				    svcrec_resp->ServiceGID.gid_guid) {
					ibcm_fill_svcinfo(svcrec_resp,
					    &dinfo->dest[j]);
					rec_found++;
				}
				if (rec_found == dinfo->num_dest)
					break;
			}
			if (rec_found == dinfo->num_dest)
				break;
		}
		if (rec_found != dinfo->num_dest) {
			IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: Did NOT "
			    "find ServiceRec for all DGIDs: (%d/%d)", rec_found,
			    dinfo->num_dest);
			retval = IBT_INSUFF_DATA;
		}
	} else if (p_arg->flags & IBT_PATH_APM) {
		ib_gid_t		p_gid, a_gid, last_p_gid;
		ib_gid_t		*gidp = NULL;
		uint_t			n_gids;
		sa_service_record_t	*stmp;
		boolean_t		pri_fill_done = B_FALSE;
		boolean_t		alt_fill_done = B_FALSE;
		ib_pkey_t		p_pkey = 0, a_pkey = 0;

		IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: Need to "
		    "find ServiceRec that can satisfy APM");

		p_gid.gid_prefix = p_gid.gid_guid = 0;
		a_gid.gid_prefix = a_gid.gid_guid = 0;
		last_p_gid.gid_prefix = last_p_gid.gid_guid = 0;

		for (i = 0; i < num_req; i++, svcrec_resp++) {
			ibt_status_t	ret;
			boolean_t	local_node_check_done = B_FALSE;

			/* Limited P_Key is NOT supported as of now!. */
			if ((svcrec_resp->ServiceP_Key & 0x8000) == 0) {
				IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: "
				    "SvcPkey 0x%X limited, reject the record.",
				    svcrec_resp->ServiceP_Key);
				continue;
			}

			p_gid = svcrec_resp->ServiceGID;

			IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: "
			    "Let DGID(%llX:%llX) be Primary",
			    p_gid.gid_prefix, p_gid.gid_guid);

			/* Let's avoid LoopBack Nodes. */
			if (p_gid.gid_guid == sl->p_sgid.gid_guid) {
				local_node_check_done = B_TRUE;

				IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: "
				    "Lets Avoid Local Node, "
				    "search for remote node.");
			}

			if (local_node_check_done == B_TRUE) {
				if ((i + 1) < num_req) {
					p_gid.gid_prefix = 0;
					p_gid.gid_guid = 0;
					continue;
				} else if (last_p_gid.gid_prefix != 0) {
					p_gid = last_p_gid;
					break;
				}
			}

			IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: "
			    "Finally let Primary DGID = %llX:%llX",
			    p_gid.gid_prefix, p_gid.gid_guid);

			ret = ibt_get_companion_port_gids(p_gid, 0, 0,
			    &gidp, &n_gids);
			if (ret == IBT_SUCCESS) {
				IBTF_DPRINTF_L3(cmlog,
				    "ibcm_saa_service_rec: Found %d "
				    "CompGID for %llX:%llX", n_gids,
				    p_gid.gid_prefix, p_gid.gid_guid);

				stmp = (sa_service_record_t *)results_p;
				a_gid.gid_prefix = a_gid.gid_guid = 0;

				if (sl->p_multi & IBTL_CM_MULTI_SM) {
					/* validate sn_pfx */
					a_gid = ibcm_saa_get_agid(sl,
					    gidp, n_gids);
				} else {
					for (k = 0; k < num_req; k++) {
						ib_gid_t sg = stmp->ServiceGID;

						IBTF_DPRINTF_L3(cmlog,
						    "ibcm_saa_service_rec: "
						    "SvcGID[%d] = %llX:%llX", k,
						    sg.gid_prefix, sg.gid_guid);

						for (j = 0; j < n_gids; j++) {
							if (gidp[j].gid_guid ==
							    sg.gid_guid) {
								a_gid = gidp[j];
								break;
							}
						}
						if (a_gid.gid_guid)
							break;
						stmp++;
					}
					if (a_gid.gid_guid == 0) {
						/* Rec not found for Alt. */
						for (j = 0; j < n_gids; j++) {
							if (gidp[j].gid_prefix
							== p_gid.gid_prefix) {
								a_gid = gidp[j];
								break;
							}
						}
					}
				}
				kmem_free(gidp,
				    n_gids * sizeof (ib_gid_t));

				if (a_gid.gid_guid)
					break;
			} else if (ret == IBT_GIDS_NOT_FOUND) {
				last_p_gid = p_gid;
				IBTF_DPRINTF_L3(cmlog,
				    "ibcm_saa_service_rec: Didn't find "
				    "CompGID for %llX:%llX, ret=%d",
				    p_gid.gid_prefix, p_gid.gid_guid,
				    ret);
			} else {
				IBTF_DPRINTF_L3(cmlog,
				    "ibcm_saa_service_rec: Call to "
				    "ibt_get_companion_port_gids(%llX:"
				    "%llX) Failed = %d",
				    p_gid.gid_prefix, p_gid.gid_guid,
				    ret);
			}
		}

		IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: \n\t"
		    "Pri DGID(%llX:%llX), Alt DGID(%llX:%llX)",
		    p_gid.gid_prefix, p_gid.gid_guid, a_gid.gid_prefix,
		    a_gid.gid_guid);

		svcrec_resp = (sa_service_record_t *)results_p;

		for (i = 0, j = 0; i < num_req; i++, svcrec_resp++) {
			/* Limited P_Key is NOT supported as of now!. */
			if ((svcrec_resp->ServiceP_Key & 0x8000) == 0) {
				IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: "
				    "SvcPkey 0x%X limited, reject the record.",
				    svcrec_resp->ServiceP_Key);
				continue;
			}

			if ((pri_fill_done == B_FALSE) &&
			    (p_gid.gid_guid ==
			    svcrec_resp->ServiceGID.gid_guid)) {
				p_pkey = svcrec_resp->ServiceP_Key;
				if ((a_pkey != 0) &&
				    (a_pkey != p_pkey)) {
					IBTF_DPRINTF_L3(cmlog,
					    "ibcm_saa_service_rec: "
					    "Pri(0x%X) & Alt (0x%X) "
					    "PKey must match.",
					    p_pkey, a_pkey);
					p_pkey = 0;
					continue;
				}
				ibcm_fill_svcinfo(svcrec_resp,
				    &dinfo->dest[j++]);
				rec_found++;
				pri_fill_done = B_TRUE;
			} else if ((alt_fill_done == B_FALSE) &&
			    (a_gid.gid_guid ==
			    svcrec_resp->ServiceGID.gid_guid)) {
				a_pkey = svcrec_resp->ServiceP_Key;
				if ((p_pkey != 0) &&
				    (a_pkey != p_pkey)) {
					IBTF_DPRINTF_L3(cmlog,
					    "ibcm_saa_service_rec: "
					    "Pri(0x%X) & Alt (0x%X) "
					    "PKey must match.",
					    p_pkey, a_pkey);
					a_pkey = 0;
					continue;
				}
				ibcm_fill_svcinfo(svcrec_resp,
				    &dinfo->dest[j++]);
				rec_found++;
				alt_fill_done = B_TRUE;
			}

			if (rec_found == 2)
				break;
		}
		if ((alt_fill_done == B_FALSE) && (a_gid.gid_guid)) {
			dinfo->dest[j].d_gid = a_gid;
			dinfo->dest[j].d_pkey = p_pkey;
			rec_found++;
			IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: "
			    "Let Alt Pkey=%X, DGID=%llX:%llX", p_pkey,
			    a_gid.gid_prefix, a_gid.gid_guid);
		}

		if (rec_found == 1)
			retval = IBT_INSUFF_DATA;
	} else if (p_arg->flags & IBT_PATH_MULTI_SVC_DEST) {
		for (i = 0; i < num_req; i++, svcrec_resp++) {
			/* Limited P_Key is NOT supported as of now!. */
			if ((svcrec_resp->ServiceP_Key & 0x8000) == 0) {
				IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: "
				    "SvcPkey 0x%X limited, reject the record.",
				    svcrec_resp->ServiceP_Key);
				continue;
			}
			ibcm_fill_svcinfo(svcrec_resp,
			    &dinfo->dest[rec_found]);
			rec_found++;
			if (rec_found == p_arg->max_paths)
				break;
		}

		if (rec_found < p_arg->max_paths)
			retval = IBT_INSUFF_DATA;
	} else {
		for (i = 0; i < num_req; i++) {
			/* Limited P_Key is NOT supported as of now!. */
			if ((svcrec_resp->ServiceP_Key & 0x8000) == 0) {
				IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: "
				    "SvcPkey 0x%X limited, reject the record.",
				    svcrec_resp->ServiceP_Key);
				svcrec_resp++;
				continue;
			}

			ibcm_fill_svcinfo(svcrec_resp, &dinfo->dest[0]);
			rec_found = 1;

			/* Avoid having loopback node */
			if (svcrec_resp->ServiceGID.gid_guid !=
			    sl->p_sgid.gid_guid) {
				break;
			} else {
				IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: "
				    "avoid LoopBack node.");
				svcrec_resp++;
			}
		}
	}

	/* Deallocate the memory for results_p. */
	kmem_free(results_p, length);
	if (dinfo->num_dest == 0)
		dinfo->num_dest = rec_found;

	/*
	 * Check out whether all Service Path we looking for are on the same
	 * P_key. If yes, then set the global p_key field with that value,
	 * to make it easy during SA Path Query.
	 */
	if ((dinfo->num_dest) && (dinfo->p_key == 0)) {
		ib_pkey_t	pk = dinfo->dest[0].d_pkey;

		if (dinfo->num_dest == 1) {
			dinfo->p_key = pk;
		} else {
			for (i = 1; i < (dinfo->num_dest - 1); i++) {
				IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: "
				    "pk= 0x%X, pk[%d]= 0x%X", pk, i,
				    dinfo->dest[i].d_pkey);
				if (pk != dinfo->dest[i].d_pkey) {
					dinfo->p_key = 0;
					break;
				} else {
					dinfo->p_key = pk;
				}
			}
		}
	}

	if (rec_found == 0) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_saa_service_rec: "
		    "ServiceRec NOT Found");
		retval = IBT_SERVICE_RECORDS_NOT_FOUND;
	}

	IBTF_DPRINTF_L3(cmlog, "ibcm_saa_service_rec: done. Status %d, "
	    "PKey 0x%X, Found %d SvcRec", retval, dinfo->p_key, rec_found);

	return (retval);
}


static boolean_t
ibcm_compare_paths(sa_path_record_t *pr_resp, ibt_cep_path_t *rc_path,
    ibtl_cm_hca_port_t *c_hp)
{
	if ((rc_path->cep_hca_port_num == c_hp->hp_port) &&
	    (rc_path->cep_adds_vect.av_src_path ==
	    (pr_resp->SLID - c_hp->hp_base_lid)) &&
	    (rc_path->cep_adds_vect.av_dlid == pr_resp->DLID) &&
	    (rc_path->cep_adds_vect.av_srate == pr_resp->Rate)) {
		return (B_TRUE);
	} else {
		return (B_FALSE);
	}
}

/*
 * ibcm_get_comp_pgids() routine gets the companion port for 'gid'.
 *
 * On success:
 *	If 'n_gid' is specified, then verify whether 'n_gid' is indeed a
 *	companion portgid of 'gid'.  If matches return success or else error.
 *
 *	If 'n_gid' is NOT specified, then return back SUCCESS along with
 *	obtained Companion PortGids 'gid_p', where 'num' indicated number
 *	of companion portgids returned in 'gid_p'.
 */

static ibt_status_t
ibcm_get_comp_pgids(ib_gid_t gid, ib_gid_t n_gid, ib_guid_t hca_guid,
    ib_gid_t **gid_p, uint_t *num)
{
	ibt_status_t    ret;
	int		i;

	ret = ibt_get_companion_port_gids(gid, hca_guid, 0, gid_p, num);
	if ((ret != IBT_SUCCESS) && (ret != IBT_GIDS_NOT_FOUND)) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_get_comp_pgids: "
		    "ibt_get_companion_port_gids(%llX:%llX) Failed: %d",
		    gid.gid_prefix, gid.gid_guid, ret);
	} else if ((ret == IBT_GIDS_NOT_FOUND) && (n_gid.gid_guid != 0)) {
		IBTF_DPRINTF_L2(cmlog, "ibcm_get_comp_pgids: Specified GID "
		    "(%llX:%llX) is NOT a Companion \n\t to current channel's "
		    "GID(%llX:%llX)", n_gid.gid_prefix, n_gid.gid_guid,
		    gid.gid_prefix, gid.gid_guid);
		ret = IBT_INVALID_PARAM;
	} else if (n_gid.gid_guid != 0) {
		/*
		 * We found some Comp GIDs and n_gid is specified. Validate
		 * whether the 'n_gid' specified is indeed the companion port
		 * GID of 'gid'.
		 */
		for (i = 0; i < *num; i++) {
			if ((n_gid.gid_prefix == gid_p[i]->gid_prefix) &&
			    (n_gid.gid_guid == gid_p[i]->gid_guid)) {
				IBTF_DPRINTF_L3(cmlog, "ibcm_get_comp_pgids: "
				    "Matching Found!. Done.");
				return (IBT_SUCCESS);
			}
		}
		IBTF_DPRINTF_L2(cmlog, "ibcm_get_comp_pgids: GID (%llX:%llX)\n"
		    "\t and (%llX:%llX) are NOT Companion Port GIDS",
		    n_gid.gid_prefix, n_gid.gid_guid, gid.gid_prefix,
		    gid.gid_guid);
		ret = IBT_INVALID_PARAM;
	} else {
		ret = IBT_SUCCESS;
	}

	IBTF_DPRINTF_L3(cmlog, "ibcm_get_comp_pgids: done. Status = %d", ret);
	return (ret);
}

/*
 * Function:
 *	ibt_get_alt_path
 * Input:
 *	rc_chan		An RC channel handle returned in a previous call
 *			ibt_alloc_rc_channel(9F), specifies the channel to open.
 *	flags		Path flags.
 *	attrp		A pointer to an ibt_alt_path_attr_t(9S) structure that
 *			specifies required attributes of the selected path(s).
 * Output:
 *	api_p		An ibt_alt_path_info_t(9S) struct filled in as output
 *			parameters.
 * Returns:
 *	IBT_SUCCESS on Success else appropriate error.
 * Description:
 *      Finds the best alternate path to a specified channel (as determined by
 *      the IBTL) that satisfies the requirements specified in an
 *      ibt_alt_path_attr_t struct.  The specified channel must have been
 *      previously opened successfully using ibt_open_rc_channel.
 *      This function also ensures that the service being accessed by the
 *      channel is available at the selected alternate port.
 *
 *      Note: The apa_dgid must be on the same destination channel adapter,
 *      if specified.
 *	This routine can not be called from interrupt context.
 */
ibt_status_t
ibt_get_alt_path(ibt_channel_hdl_t rc_chan, ibt_path_flags_t flags,
    ibt_alt_path_attr_t *attrp, ibt_alt_path_info_t *api_p)
{
	sa_multipath_record_t	*mpr_req;
	sa_path_record_t	*pr_resp;
	ibmf_saa_access_args_t	access_args;
	ibt_qp_query_attr_t	qp_attr;
	ibtl_cm_hca_port_t	c_hp, n_hp;
	ibcm_hca_info_t		*hcap;
	void			*results_p;
	uint64_t		c_mask = 0;
	ib_gid_t		*gid_ptr = NULL;
	ib_gid_t		*sgids_p = NULL,  *dgids_p = NULL;
	ib_gid_t		cur_dgid, cur_sgid;
	ib_gid_t		new_dgid, new_sgid;
	ibmf_saa_handle_t	saa_handle;
	size_t			length;
	int			i, j, template_len, rec_found;
	uint_t			snum = 0, dnum = 0, num_rec;
	ibt_status_t		retval;
	ib_mtu_t		prim_mtu;

	IBTF_DPRINTF_L3(cmlog, "ibt_get_alt_path(%p, %x, %p, %p)",
	    rc_chan, flags, attrp, api_p);

	/* validate channel */
	if (IBCM_INVALID_CHANNEL(rc_chan)) {
		IBTF_DPRINTF_L2(cmlog, "ibt_get_alt_path: invalid channel");
		return (IBT_CHAN_HDL_INVALID);
	}

	if (api_p == NULL) {
		IBTF_DPRINTF_L2(cmlog, "ibt_get_alt_path: invalid attribute: "
		    " AltPathInfo can't be NULL");
		return (IBT_INVALID_PARAM);
	}

	retval = ibt_query_qp(rc_chan, &qp_attr);
	if (retval != IBT_SUCCESS) {
		IBTF_DPRINTF_L2(cmlog, "ibt_get_alt_path: ibt_query_qp(%p) "
		    "failed %d", rc_chan, retval);
		return (retval);
	}

	if (qp_attr.qp_info.qp_trans != IBT_RC_SRV) {
		IBTF_DPRINTF_L2(cmlog, "ibt_get_alt_path: "
		    "Invalid Channel type: Applicable only to RC Channel");
		return (IBT_CHAN_SRV_TYPE_INVALID);
	}

	cur_dgid =
	    qp_attr.qp_info.qp_transport.rc.rc_path.cep_adds_vect.av_dgid;
	cur_sgid =
	    qp_attr.qp_info.qp_transport.rc.rc_path.cep_adds_vect.av_sgid;
	prim_mtu = qp_attr.qp_info.qp_transport.rc.rc_path_mtu;

	/* If optional attributes are specified, validate them. */
	if (attrp) {
		new_dgid = attrp->apa_dgid;
		new_sgid = attrp->apa_sgid;
	} else {
		new_dgid.gid_prefix = 0;
		new_dgid.gid_guid = 0;
		new_sgid.gid_prefix = 0;
		new_sgid.gid_guid = 0;
	}

	if ((new_dgid.gid_prefix != 0) && (new_sgid.gid_prefix != 0) &&
	    (new_dgid.gid_prefix != new_sgid.gid_prefix)) {
		IBTF_DPRINTF_L2(cmlog, "ibt_get_alt_path: Specified SGID's "
		    "SNprefix (%llX) doesn't match with \n specified DGID's "
		    "SNprefix: %llX", new_sgid.gid_prefix, new_dgid.gid_prefix);
		return (IBT_INVALID_PARAM);
	}

	/* For the specified SGID, get HCA information. */
	retval = ibtl_cm_get_hca_port(cur_sgid, 0, &c_hp);
	if (retval != IBT_SUCCESS) {
		IBTF_DPRINTF_L2(cmlog, "ibt_get_alt_path: "
		    "Get HCA Port Failed: %d", retval);
		return (retval);
	}

	hcap = ibcm_find_hca_entry(c_hp.hp_hca_guid);
	if (hcap == NULL) {
		IBTF_DPRINTF_L2(cmlog, "ibt_get_alt_path: NO HCA found");
		return (IBT_HCA_BUSY_DETACHING);
	}

	/* Validate whether this HCA support APM */
	if (!(hcap->hca_caps & IBT_HCA_AUTO_PATH_MIG)) {
		IBTF_DPRINTF_L2(cmlog, "ibt_get_alt_path: "
		    "HCA (%llX) - APM NOT SUPPORTED ", c_hp.hp_hca_guid);
		retval = IBT_APM_NOT_SUPPORTED;
		goto get_alt_path_done;
	}

	/* Get Companion Port GID of the current Channel's SGID */
	if ((new_sgid.gid_guid == 0) || ((new_sgid.gid_guid != 0) &&
	    (new_sgid.gid_guid != cur_sgid.gid_guid))) {
		IBTF_DPRINTF_L3(cmlog, "ibt_get_alt_path: SRC: "
		    "Get Companion PortGids for - %llX:%llX",
		    cur_sgid.gid_prefix, cur_sgid.gid_guid);

		retval = ibcm_get_comp_pgids(cur_sgid, new_sgid,
		    c_hp.hp_hca_guid, &sgids_p, &snum);
		if (retval != IBT_SUCCESS)
			goto get_alt_path_done;
	}

	/* Get Companion Port GID of the current Channel's DGID */
	if ((new_dgid.gid_guid == 0) || ((new_dgid.gid_guid != 0) &&
	    (new_dgid.gid_guid != cur_dgid.gid_guid))) {

		IBTF_DPRINTF_L3(cmlog, "ibt_get_alt_path: DEST: "
		    "Get Companion PortGids for - %llX:%llX",
		    cur_dgid.gid_prefix, cur_dgid.gid_guid);

		retval = ibcm_get_comp_pgids(cur_dgid, new_dgid, 0, &dgids_p,
		    &dnum);
		if (retval != IBT_SUCCESS)
			goto get_alt_path_done;
	}

	if ((new_dgid.gid_guid == 0) || (new_sgid.gid_guid == 0)) {
		if (new_sgid.gid_guid == 0) {
			for (i = 0; i < snum; i++) {
				if (new_dgid.gid_guid == 0) {
					for (j = 0; j < dnum; j++) {
						if (sgids_p[i].gid_prefix ==
						    dgids_p[j].gid_prefix) {
							new_dgid = dgids_p[j];
							new_sgid = sgids_p[i];

							goto get_alt_proceed;
						}
					}
					/*  Current DGID */
					if (sgids_p[i].gid_prefix ==
					    cur_dgid.gid_prefix) {
						new_sgid = sgids_p[i];
						goto get_alt_proceed;
					}
				} else {
					if (sgids_p[i].gid_prefix ==
					    new_dgid.gid_prefix) {
						new_sgid = sgids_p[i];
						goto get_alt_proceed;
					}
				}
			}
			/* Current SGID */
			if (new_dgid.gid_guid == 0) {
				for (j = 0; j < dnum; j++) {
					if (cur_sgid.gid_prefix ==
					    dgids_p[j].gid_prefix) {
						new_dgid = dgids_p[j];

						goto get_alt_proceed;
					}
				}
			}
		} else if (new_dgid.gid_guid == 0) {
			for (i = 0; i < dnum; i++) {
				if (dgids_p[i].gid_prefix ==
				    new_sgid.gid_prefix) {
					new_dgid = dgids_p[i];
					goto get_alt_proceed;
				}
			}
			/* Current DGID */
			if (cur_dgid.gid_prefix == new_sgid.gid_prefix) {
				goto get_alt_proceed;
			}
		}
		/*
		 * hmm... No Companion Ports available.
		 * so we will be using current or specified attributes only.
		 */
	}

get_alt_proceed:

	if (new_sgid.gid_guid != 0) {
		retval = ibtl_cm_get_hca_port(new_sgid, 0, &n_hp);
		if (retval != IBT_SUCCESS) {
			IBTF_DPRINTF_L2(cmlog, "ibt_get_alt_path: "
			    "Get HCA Port Failed: %d", retval);
			goto get_alt_path_done;
		}
	}

	/* Calculate the size for multi-path records template */
	template_len = (2 * sizeof (ib_gid_t)) + sizeof (sa_multipath_record_t);

	mpr_req = kmem_zalloc(template_len, KM_SLEEP);

	ASSERT(mpr_req != NULL);

	_NOTE(NOW_INVISIBLE_TO_OTHER_THREADS(*mpr_req))

	gid_ptr = (ib_gid_t *)(((uchar_t *)mpr_req) +
	    sizeof (sa_multipath_record_t));

	/* SGID */
	if (new_sgid.gid_guid == 0)
		*gid_ptr = cur_sgid;
	else
		*gid_ptr = new_sgid;

	IBTF_DPRINTF_L3(cmlog, "ibt_get_alt_path: Get Path Between "
	    " SGID : %llX:%llX", gid_ptr->gid_prefix, gid_ptr->gid_guid);

	gid_ptr++;

	/* DGID */
	if (new_dgid.gid_guid == 0)
		*gid_ptr = cur_dgid;
	else
		*gid_ptr = new_dgid;

	IBTF_DPRINTF_L3(cmlog, "ibt_get_alt_path:\t\t    DGID : %llX:%llX",
	    gid_ptr->gid_prefix, gid_ptr->gid_guid);

	mpr_req->SGIDCount = 1;
	c_mask = SA_MPR_COMPMASK_SGIDCOUNT;

	mpr_req->DGIDCount = 1;
	c_mask |= SA_MPR_COMPMASK_DGIDCOUNT;

	/* Is Flow Label Specified. */
	if (attrp) {
		if (attrp->apa_flow) {
			mpr_req->FlowLabel = attrp->apa_flow;
			c_mask |= SA_MPR_COMPMASK_FLOWLABEL;
		}

		/* Is HopLimit Specified. */
		if (flags & IBT_PATH_HOP) {
			mpr_req->HopLimit = attrp->apa_hop;
			c_mask |= SA_MPR_COMPMASK_HOPLIMIT;
		}

		/* Is TClass Specified. */
		if (attrp->apa_tclass) {
			mpr_req->TClass = attrp->apa_tclass;
			c_mask |= SA_MPR_COMPMASK_TCLASS;
		}

		/* Is SL specified. */
		if (attrp->apa_sl) {
			mpr_req->SL = attrp->apa_sl;
			c_mask |= SA_MPR_COMPMASK_SL;
		}

		if (flags & IBT_PATH_PERF) {
			mpr_req->PacketLifeTimeSelector = IBT_BEST;
			mpr_req->RateSelector = IBT_BEST;

			c_mask |= SA_MPR_COMPMASK_PKTLTSELECTOR |
			    SA_MPR_COMPMASK_RATESELECTOR;
		} else {
			if (attrp->apa_pkt_lt.p_selector == IBT_BEST) {
				mpr_req->PacketLifeTimeSelector = IBT_BEST;
				c_mask |= SA_MPR_COMPMASK_PKTLTSELECTOR;
			}

			if (attrp->apa_srate.r_selector == IBT_BEST) {
				mpr_req->RateSelector = IBT_BEST;
				c_mask |= SA_MPR_COMPMASK_RATESELECTOR;
			}
		}

		/*
		 * Honor individual selection of these attributes,
		 * even if IBT_PATH_PERF is set.
		 */
		/* Check out whether Packet Life Time is specified. */
		if (attrp->apa_pkt_lt.p_pkt_lt) {
			mpr_req->PacketLifeTime =
			    ibt_usec2ib(attrp->apa_pkt_lt.p_pkt_lt);
			mpr_req->PacketLifeTimeSelector =
			    attrp->apa_pkt_lt.p_selector;

			c_mask |= SA_MPR_COMPMASK_PKTLT |
			    SA_MPR_COMPMASK_PKTLTSELECTOR;
		}

		/* Is SRATE specified. */
		if (attrp->apa_srate.r_srate) {
			mpr_req->Rate = attrp->apa_srate.r_srate;
			mpr_req->RateSelector = attrp->apa_srate.r_selector;

			c_mask |= SA_MPR_COMPMASK_RATE |
			    SA_MPR_COMPMASK_RATESELECTOR;
		}
	}

	/* Alt PathMTU can be GT or EQU to current channel's Pri PathMTU */

	/* P_Key must be same as that of primary path */
	retval = ibt_index2pkey_byguid(c_hp.hp_hca_guid, c_hp.hp_port,
	    qp_attr.qp_info.qp_transport.rc.rc_path.cep_pkey_ix,
	    &mpr_req->P_Key);
	if (retval != IBT_SUCCESS) {
		IBTF_DPRINTF_L2(cmlog, "ibt_get_alt_path: Idx2Pkey Failed: %d",
		    retval);
		goto get_alt_path_done;
	}
	c_mask |= SA_MPR_COMPMASK_PKEY;

	mpr_req->Reversible = 1;	/* We always get REVERSIBLE paths. */
	mpr_req->IndependenceSelector = 1;
	c_mask |= SA_MPR_COMPMASK_REVERSIBLE | SA_MPR_COMPMASK_INDEPSEL;

	_NOTE(NOW_VISIBLE_TO_OTHER_THREADS(*mpr_req))

	IBTF_DPRINTF_L3(cmlog, "ibt_get_alt_path: CMask: 0x%llX", c_mask);

	/* NOTE: We will **NOT** specify how many records we want. */

	IBTF_DPRINTF_L3(cmlog, "ibt_get_alt_path: Primary: MTU %d, PKey[%d]="
	    "0x%X\n\tSGID = %llX:%llX, DGID = %llX:%llX", prim_mtu,
	    qp_attr.qp_info.qp_transport.rc.rc_path.cep_pkey_ix, mpr_req->P_Key,
	    cur_sgid.gid_prefix, cur_sgid.gid_guid, cur_dgid.gid_prefix,
	    cur_dgid.gid_guid);

	/* Get SA Access Handle. */
	if (new_sgid.gid_guid != 0)
		saa_handle = ibcm_get_saa_handle(hcap, n_hp.hp_port);
	else
		saa_handle = ibcm_get_saa_handle(hcap, c_hp.hp_port);
	if (saa_handle == NULL) {
		retval = IBT_HCA_PORT_NOT_ACTIVE;
		goto get_alt_path_done;
	}

	/* Contact SA Access to retrieve Path Records. */
	access_args.sq_attr_id = SA_MULTIPATHRECORD_ATTRID;
	access_args.sq_access_type = IBMF_SAA_RETRIEVE;
	access_args.sq_component_mask = c_mask;
	access_args.sq_template = mpr_req;
	access_args.sq_template_length = sizeof (sa_multipath_record_t);
	access_args.sq_callback = NULL;
	access_args.sq_callback_arg = NULL;

	retval = ibcm_contact_sa_access(saa_handle, &access_args, &length,
	    &results_p);
	if (retval != IBT_SUCCESS) {
		goto get_alt_path_done;
	}

	num_rec = length / sizeof (sa_path_record_t);

	kmem_free(mpr_req, template_len);

	IBTF_DPRINTF_L3(cmlog, "ibt_get_alt_path: Found %d Paths", num_rec);

	rec_found = 0;
	if ((results_p != NULL) && (num_rec > 0)) {
		/* Update the PathInfo with the response Path Records */
		pr_resp = (sa_path_record_t *)results_p;
		for (i = 0; i < num_rec; i++, pr_resp++) {
			if (prim_mtu > pr_resp->Mtu) {
				IBTF_DPRINTF_L2(cmlog, "ibt_get_alt_path: "
				    "Alt PathMTU(%d) must be GT or EQU to Pri "
				    "PathMTU(%d). Ignore this rec",
				    pr_resp->Mtu, prim_mtu);
				continue;
			}

			if ((new_sgid.gid_guid == 0) &&
			    (new_dgid.gid_guid == 0)) {
				/* Reject PathRec if it same as Primary Path. */
				if (ibcm_compare_paths(pr_resp,
				    &qp_attr.qp_info.qp_transport.rc.rc_path,
				    &c_hp) == B_TRUE) {
					IBTF_DPRINTF_L3(cmlog,
					    "ibt_get_alt_path: PathRec obtained"
					    " is similar to Prim Path, ignore "
					    "this record");
					continue;
				}
			}

			if (new_sgid.gid_guid == 0) {
				retval = ibcm_update_cep_info(pr_resp, NULL,
				    &c_hp, &api_p->ap_alt_cep_path);
			} else {
				retval = ibcm_update_cep_info(pr_resp, NULL,
				    &n_hp, &api_p->ap_alt_cep_path);
			}
			if (retval != IBT_SUCCESS)
				continue;

			/* Update some leftovers */
			_NOTE(NOW_INVISIBLE_TO_OTHER_THREADS(*api_p))

			api_p->ap_alt_pkt_lt = pr_resp->PacketLifeTime;

			_NOTE(NOW_VISIBLE_TO_OTHER_THREADS(*api_p))

			rec_found = 1;
			break;
		}
		kmem_free(results_p, length);
	}

	if (rec_found == 0) {
		IBTF_DPRINTF_L3(cmlog, "ibt_get_alt_path: Alternate Path cannot"
		    " be established");
		retval = IBT_PATH_RECORDS_NOT_FOUND;
	} else
		retval = IBT_SUCCESS;

get_alt_path_done:
	if ((snum) && (sgids_p))
		kmem_free(sgids_p, snum * sizeof (ib_gid_t));

	if ((dnum) && (dgids_p))
		kmem_free(dgids_p, dnum * sizeof (ib_gid_t));

	ibcm_dec_hca_acc_cnt(hcap);

	IBTF_DPRINTF_L3(cmlog, "ibt_get_alt_path: Done (status %d).", retval);

	return (retval);
}


/* Routines for warlock */

/* ARGSUSED */
static void
ibcm_dummy_path_handler(void *arg, ibt_status_t retval, ibt_path_info_t *paths,
    uint8_t num_path)
{
	ibcm_path_tqargs_t	dummy_path;

	dummy_path.func = ibcm_dummy_path_handler;

	IBTF_DPRINTF_L5(cmlog, "ibcm_dummy_path_handler: "
	    "dummy_path.func %p", dummy_path.func);
}
