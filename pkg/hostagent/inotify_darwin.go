package hostagent

import "github.com/rjeczalik/notify"

func GetNotifyEvent() notify.Event {
	return notify.Create | notify.Write | notify.Remove | notify.Rename | notify.FSEventsInodeMetaMod // TODO: linux, others
	// return notify.Create | notify.Write | notify.FSEventsInodeMetaMod
}
