export default function Modal({ title, message, details, buttons = [], onClose }) {
  return (
    <div className="modal-overlay" onClick={() => onClose(false)}>
      <div className="modal-box" onClick={(e) => e.stopPropagation()}>
        <h2 className="modal-title">{title}</h2>
        <div className="modal-message">{message}</div>
        {details && <div className="modal-details">{details}</div>}
        <div className="modal-actions">
          {buttons.map((btn, idx) => (
            <button key={idx} className={btn.primary ? 'primary' : ''} onClick={() => onClose(btn.value)}>
              {btn.label}
            </button>
          ))}
        </div>
      </div>
    </div>
  )
}
