function flash() {
    const id = location.hash.substring(1)
    if (id) {
        const el = document.getElementById(id)
        if (el) {
            el.classList.remove('flash')
            void el.offsetWidth // Force reflow so the animation can restart
            el.classList.add('flash')
        }
    }
}

window.onload = flash
window.onhashchange = flash
