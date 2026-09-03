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
