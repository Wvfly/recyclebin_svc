package api

import (
	"strings"
	"sync"
	"unsafe"

	"rbapi/db"

	"golang.org/x/sys/windows"
)

// sidCache memoizes SID -> account name so we do not call the Win32 resolver
// on every row of every list response. Value "" means "resolved but unknown"
// (placeholder SID or an account the server could not translate) and is cached
// too, so repeated lookups stay cheap.
var sidCache sync.Map // map[sid] -> string

// lookupSidName returns the "DOMAIN\account" name for a stored SID, or "" if
// the SID is empty, a kernel session placeholder (S-SESSION-<id>), or cannot
// be resolved on this host. The result is cached.
func lookupSidName(sid string) string {
	if sid == "" || strings.HasPrefix(sid, "S-SESSION-") {
		return ""
	}
	if v, ok := sidCache.Load(sid); ok {
		return v.(string)
	}
	name := resolveSidOnce(sid)
	sidCache.Store(sid, name)
	return name
}

// resolveSidOnce converts a SID string to an account name via
// ConvertStringSidToSid + LookupAccount. It is the uncached slow path.
func resolveSidOnce(sid string) string {
	var psid *windows.SID
	if err := windows.ConvertStringSidToSid(windows.StringToUTF16Ptr(sid), &psid); err != nil {
		return ""
	}
	defer windows.LocalFree(windows.Handle(uintptr(unsafe.Pointer(psid))))

	acc, dom, _, err := psid.LookupAccount("")
	if err != nil {
		return ""
	}
	if dom != "" {
		return dom + `\` + acc
	}
	return acc
}

// fillUsernames resolves Sid -> Username for a batch of items in place.
func fillUsernames(items []db.Item) {
	for i := range items {
		items[i].Username = lookupSidName(items[i].Sid)
	}
}

// buildItemFilter turns a status and a "SID or username" string into a filter.
//
// The schema stores only SIDs; usernames exist nowhere in the database, they
// are resolved at query time. So a username cannot be matched in SQL directly.
// Instead we resolve it to the SIDs that belong to it and filter by that set,
// which keeps the WHERE clause (and therefore LIMIT/OFFSET and COUNT) correct.
//
// Interpretation of userOrSid:
//   - ""            -> no SID filter
//   - "S-..."       -> SID, matched as a prefix so a partial SID works
//   - anything else -> username fragment, matched case-insensitively against
//                      "DOMAIN\\account" (so "alice" and "CORP\\alice" both work)
//
// A username that resolves to no known account yields Sids = []string{}, i.e.
// match nothing. Returning "no filter" there would silently show every row,
// which looks like the filter was ignored.
func (s *Server) buildItemFilter(status, userOrSid string) (db.ItemFilter, error) {
	f := db.ItemFilter{Status: status}
	if userOrSid == "" {
		return f, nil
	}
	if strings.HasPrefix(userOrSid, "S-") {
		f.SidPrefix = userOrSid
		return f, nil
	}

	if s.DB == nil {
		// No database (tests, or a request dbUnavailable() already rejected).
		// Match nothing rather than panicking, and never fall back to "no filter".
		f.Sids = []string{}
		return f, nil
	}
	sids, err := s.DB.DistinctSids()
	if err != nil {
		return f, err
	}
	f.Sids = sidsMatchingUser(sids, userOrSid)
	return f, nil
}

// sidsMatchingUser returns the SIDs whose resolved account name contains
// needle (case-insensitive). SIDs that do not resolve to a name are skipped.
func sidsMatchingUser(sids []string, needle string) []string {
	want := strings.ToLower(needle)
	out := []string{} // non-nil: "resolved to zero accounts" != "no filter"
	for _, sid := range sids {
		name := lookupSidName(sid)
		if name == "" {
			continue
		}
		if strings.Contains(strings.ToLower(name), want) {
			out = append(out, sid)
		}
	}
	return out
}
