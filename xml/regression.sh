#!/bin/sh
# Copyright 2018 Red Hat, Inc.
# Author: Jan Pokorny <jpokorny@redhat.com>
# Part of pacemaker project
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu
DIFF=${DIFF:-diff}
DIFFOPTS=${DIFFOPTS:--u}
DIFFPAGER=${DIFFPAGER:-less -LRX}
tests=

#
# commons
#

emit_result() {
	_er_howmany=${1:?}  # how many errors (0/anything else incl. strings)
	_er_subject=${2:?}
	_er_prefix=${3-}

	test -z "${_er_prefix}" || _er_prefix="${_er_prefix}: "

	if test "${_er_howmany}" = 0; then
		printf "%s%s finished OK\n" "${_er_prefix}" "${_er_subject}"
	else
		printf "%s%s encountered ${_er_howmany} errors\n" \
		       "${_er_prefix}" "${_er_subject}"
	fi
}

emit_error() {
	_ee_msg=${1:?}
	printf "%s\n" "${_ee_msg}" >&2
}

# returns 1 + floor of base 2 logaritm for _lo0r_i in 1...255,
# or 0 for _lo0r_i = 0
log2_or_0_return() {
	_lo0r_i=${1:?}
	return $(((!(_lo0r_i >> 1) && _lo0r_i) * 1 \
                + (!(_lo0r_i >> 2) && _lo0r_i & (1 << 1)) * 2 \
                + (!(_lo0r_i >> 3) && _lo0r_i & (1 << 2)) * 3 \
                + (!(_lo0r_i >> 4) && _lo0r_i & (1 << 3)) * 4 \
                + (!(_lo0r_i >> 5) && _lo0r_i & (1 << 4)) * 5 \
                + (!(_lo0r_i >> 6) && _lo0r_i & (1 << 5)) * 6 \
                + (!(_lo0r_i >> 7) && _lo0r_i & (1 << 6)) * 7 \
                + !!(_lo0r_i >> 7) * 7 ))
}

# rough addition of two base 2 logarithms
log2_or_0_add() {
	_lo0a_op1=${1:?}
	_lo0a_op2=${2:?}

	if test ${_lo0a_op1} -gt ${_lo0a_op2}; then
		return ${_lo0a_op1}
	elif test ${_lo0a_op2} -gt ${_lo0a_op1}; then
		return ${_lo0a_op2}
	elif test ${_lo0a_op1} -gt 0; then
		return $((_lo0a_op1 + 1))
	else
		return ${_lo0a_op1}
	fi
}

#
# test phases
#

# stdin: input file per line
test_cleaner() {
	while read _tc_origin; do
		rm -f ${_tc_origin%.*}.up ${_tc_origin%.*}.up.err
	done
}

test_selfcheck() {
	_tsc_template=
	_tsc_validator=

	while test $# -gt 0; do
		case "$1" in
		-o=*) _tsc_template="${1#-o=}";;
		esac
		shift
	done
	_tsc_validator="${_tsc_template:?}"
	_tsc_validator="xslt_cibtr-${_tsc_validator%%.*}.rng"
	_tsc_template="upgrade-${_tsc_template}.xsl"

	xmllint --noout --relaxng "${_tsc_validator}" "${_tsc_template}"
}

# stdout: filename of the transformed file
test_runner_upgrade() {
	_tru_template=${1:?}
	_tru_source=${2:?}  # filename
	_tru_mode=${3:?}  # extra modes wrt. "referential" outcome, see below

	_tru_ref="${_tru_source%.*}.ref"
        { test "${_tru_mode}" -ne 0 || test -f "${_tru_ref}.err"; } \
	  && _tru_ref_err="${_tru_ref}.err" \
	  || _tru_ref_err=/dev/null
	_tru_target="${_tru_source%.*}.up"
	_tru_target_err="${_tru_target}.err"

	xsltproc "${_tru_template}" "${_tru_source}" \
	  > "${_tru_target}" 2> "${_tru_target_err}" \
	  || { _tru_ref=$?; echo "${_tru_target_err}"; return ${_tru_ref}; }

	if test "${_tru_mode}" -ne 0; then
		if test $((_tru_mode & (1 << 0))) -ne 0; then
			cp -a "${_tru_target}" "${_tru_ref}"
			cp -a "${_tru_target_err}" "${_tru_ref_err}"
		fi
		if test $((_tru_mode & (1 << 1))) -ne 0; then
			"${DIFF}"  ${DIFFOPTS} "${_tru_source}" "${_tru_ref}" \
			  | ${DIFFPAGER} >&2
			if test $? -ne 0; then
				printf "\npager failure\n" >&2
				return 1
			fi
			printf '\nIs comparison OK? ' >&2
			read _tru_answer </dev/tty
			case "${_tru_answer}" in
			y|yes) ;;
			*) echo "Answer not 'y' nor 'yes'" >&2; return 1;;
			esac
		fi
	elif test -f "${_tru_ref}" && test -f "${_tru_ref_err}"; then
		"${DIFF}"  ${DIFFOPTS} "${_tru_ref}" "${_tru_target}" >&2 \
		  && "${DIFF}"  ${DIFFOPTS} "${_tru_ref_err}" "${_tru_target_err}" >&2
		if test $? -ne 0; then
			emit_error "Outputs differ from referential ones"
			echo "/dev/null"
			return 1
		fi
	else
		emit_error "Referential file(s) missing: ${_tru_ref}"
		echo "/dev/null"
		return 1
	fi

	echo "${_tru_target}"
}

test_runner_validate() {
	_trv_schema=${1:?}
	_trv_target=${2:?}  # filename

	if ! xmllint --noout --relaxng "${_trv_schema}" "${_trv_target}" \
	    2>/dev/null; then
		xmllint --noout --relaxng "${_trv_schema}" "${_trv_target}"
	fi
}

# -g  ... request generating "referential" outcomes
# -o= ... which conventional version to deem as the transform origin
# -t= ... which conventional version to deem as the transform target
# -D
# -G  ... see usage
# stdin: input file per line
test_runner() {
	_tr_mode=0
	_tr_ret=0
	_tr_schema_o=
	_tr_schema_t=
	_tr_target=
	_tr_template=

	while test $# -gt 0; do
		case "$1" in
		-o=*) _tr_template="upgrade-${1#-o=}.xsl"
		      _tr_schema_o="pacemaker-${1#-o=}.rng";;
		-t=*) _tr_schema_t="pacemaker-${1#-t=}.rng";;
		-G) _tr_mode=$((_tr_mode | (1 << 0)));;
		-D) _tr_mode=$((_tr_mode | (1 << 1)));;
		esac
		shift
	done

	if ! test -f "${_tr_schema_o:?}" || ! test -f "${_tr_schema_t:?}"; then
		emit_error "Origin and/or target schema missing, rerun make"
		return 1
	fi

	while read _tr_origin; do
		printf '%-60s' "${_tr_origin}... "

		# pre-validate
		if ! test_runner_validate "${_tr_schema_o}" "${_tr_origin}"; then
			_tr_ret=$((_tr_ret + 1)); echo "E:pre-validate"; continue
		fi

		# upgrade
		if ! _tr_target=$(test_runner_upgrade "${_tr_template}" \
		                 "${_tr_origin}" "${_tr_mode}"); then
			_tr_ret=$((_tr_ret + 1));
			test -n "${_tr_target}" || break
			echo "E:upgrade"
			test -s "${_tr_target}" \
			  && { echo ---; cat "${_tr_target}" || :; echo ---; }
			continue
		fi

		# post-validate
		if ! test_runner_validate "${_tr_schema_t}" "${_tr_target}"; then
			_tr_ret=$((_tr_ret + 1)); echo "E:post-validate"; continue
		fi

		echo "OK"
	done

	log2_or_0_return ${_tr_ret}
}

#
# particular test variations
#

test_2to3() {
	find test-2 -name '*.xml' -print | env LC_ALL=C sort \
	  | { case " $* " in
	      *\ -C\ *) test_cleaner;;
	      *\ -S\ *) test_selfcheck -o=2.10;;
	      *) test_runner -o=2.10 -t=3.0 "$@" || return $?;;
	      esac; }
}
tests="${tests} test_2to3"

#
# "framework"
#

# option-likes ... options to be passed down
# argument-likes ... drives a test selection
test_suite() {
	_ts_pass=
	_ts_global_ret=0
	_ts_ret=0
	_ts_select=@

	while test $# -gt 0; do
		case "$1" in
		-*) _ts_pass="${_ts_pass} $1";;
		*) _ts_select="${_ts_select}@$1";;
		esac
		shift
	done
	_ts_select="${_ts_select}@"

	for _ts_test in ${tests}; do
		if test "${_ts_select}" != @@; then
			case "${_ts_test}" in
			*@${_ts_select}@*) ;;  # jump through
			*) break;;
			esac
		fi
		"${_ts_test}" ${_ts_pass} || _ts_ret=$?
		test ${_ts_ret} = 0 \
		  && emit_result ${_ts_ret} "${_ts_test}" \
		  || emit_result "at least 2^$((_ts_ret - 1))" "${_ts_test}"
		log2_or_0_add ${_ts_global_ret} ${_ts_ret}
		_ts_global_ret=$?
	done

	return ${_ts_global_ret}
}

# NOTE: big letters are dedicated for per-test-set behaviour,
#       small ones for generic/global behaviour
usage() {
	printf '%s\n  %s\n  %s\n  %s\n  %s\n' \
	    "usage: $0 [-{C,G,S}]* [{${tests## }}*]" \
	    "- use '-C' to only cleanup ephemeral byproducts" \
	    "- use '-D' to review originals vs. \"referential\" outcomes" \
	    "- use '-G' to generate \"referential\" outcomes" \
	    "- use '-S' for template self-check (requires net access)"
}

main() {
	_main_pass=
	_main_bailout=0
	_main_ret=0

	while test $# -gt 0; do
		case "$1" in
		-h) usage; exit;;
		-C) _main_pass="${_main_pass} $1"; _main_bailout=1;;
		-S) _main_pass="${_main_pass} $1"; _main_bailout=1;;
		*) _main_pass="${_main_pass} $1";;
		esac
		shift
	done

	test_suite ${_main_pass} || _main_ret=$?
	test ${_main_bailout} -eq 1 && return ${_main_ret} \
	  || test_suite -C ${_main_pass} >/dev/null
	test ${_main_ret} = 0 && emit_result ${_main_ret} "Overall suite" \
	  || emit_result "at least 2^$((_main_ret - 1))" "Overall suite"

	return ${_main_ret}
}

main "$@"
