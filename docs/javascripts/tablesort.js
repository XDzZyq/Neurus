// Material for MkDocs loads pages through instant navigation, so a script that
// runs once at page load only ever sees the first page. `document$` is the
// observable Material re-emits on every navigation; subscribing per page keeps
// tables sortable after a client-side jump.
document$.subscribe(function ()
{
	var tables = document.querySelectorAll("article table:not([class])")
	tables.forEach(function (table)
	{
		new Tablesort(table)
	})
})
